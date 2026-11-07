#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <uv.h>

#include "uvco/core/cancel.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"

// Timer：可重复的定时器句柄（docs/architecture.md §4.4），只可移动。
//
//   uvco::Timer timer{loop};
//   timer.start(100ms, 100ms);                    // 首次 100ms 后，之后每 100ms
//   for (...) {
//       CO2_AWAIT_SET(r, timer.tick());           // Result<std::uint64_t>
//       // r.value()：自上次 tick() 以来到期的次数（≥1；没人等待时到期会累计，不丢失）
//   }
//   CO2_AWAIT(timer.close());                     // 可选：等待 uv_close 完成
//
// tick() 的结果：
//   - 已有累计的到期：立即完成，返回累计次数并清零；
//   - 计时器未启动（或一次性计时器已到期并被取走）：立即返回 UV_EINVAL；
//   - 否则挂起直到下一次到期。同一时刻只能有一个 tick() 在等待（契约）。
//   - 取消：stop_token 被请求 → UV_ECANCELED（计时器本身继续运行，累计不丢失）；
//     stop()、close() 或 Loop 析构时挂起中的 tick() 以 Errc::aborted 结束。
//
// tick() 返回的 awaiter 只含一个指针（放进 co2 的内联槽，零分配），应立即等待。
// 生命周期见 core/handle.hpp：析构时若有挂起的 tick() 是契约违规。

namespace uvco {

class Timer;

namespace detail {

inline std::uint64_t toMilliseconds(std::chrono::milliseconds const duration) noexcept {
    UVCO_CONTRACT_CHECK(duration.count() >= 0);
    return static_cast<std::uint64_t>(duration.count());
}

class TimerBlock final : public HandleBlock {
  public:
    explicit TimerBlock(Loop& loop) noexcept : HandleBlock{loop} {
        int const status = uv_timer_init(loop.raw(), &timer_);
        UVCO_CONTRACT_CHECK(status == 0);
        bindHandle();
    }

    uv_timer_t* raw() noexcept { return &timer_; }

    Result<void> start(std::uint64_t const timeout, std::uint64_t const repeat) noexcept {
        expirations_ = 0;
        uv_update_time(loop().raw()); // 从调用时刻起算（见 sleep.hpp）
        return fromStatus(uv_timer_start(&timer_, &TimerBlock::onTimer, timeout, repeat));
    }

    void stop() noexcept {
        static_cast<void>(uv_timer_stop(&timer_));
        expirations_ = 0;
        if (state_ == State::pending) finishTick(UVCO_EABORTED, Resume::later);
    }

    // ---- tick() awaiter ----

    bool suspendTick(co2::coroutine_handle<> const waiter, co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        UVCO_CONTRACT_CHECK(state_ == State::idle); // 一次只能有一个 tick() 等待
        state_ = State::done;
        if (token.stop_requested()) {
            status_ = UV_ECANCELED;
            return false;
        }
        if (expirations_ != 0) {
            status_ = 0;
            taken_ = std::exchange(expirations_, 0);
            return false;
        }
        if (uv_is_active(reinterpret_cast<uv_handle_t const*>(&timer_)) == 0) {
            status_ = UV_EINVAL; // 未启动：永远不会到期
            return false;
        }
        state_ = State::pending;
        waiter_ = waiter;
        suspended_ = true;
        opBegin();
        cancel_.arm(loop(), std::move(token), &TimerBlock::onCancel, this);
        return true;
    }

    Result<std::uint64_t> resumeTick() noexcept {
        UVCO_CONTRACT_CHECK(state_ == State::done);
        cancel_.disarm();
        Result<std::uint64_t> result = status_ < 0 ? Result<std::uint64_t>{static_cast<Errc>(status_)}
                                                   : Result<std::uint64_t>{taken_};
        state_ = State::idle;
        waiter_ = nullptr;
        taken_ = 0;
        if (std::exchange(suspended_, false)) opEnd(); // 之后 this 可能已被释放
        return result;
    }

  private:
    enum class State : unsigned char { idle, pending, done };
    enum class Resume : unsigned char { inlineNow, later };

    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&timer_); }

    void abortPending() noexcept override {
        if (state_ == State::pending) finishTick(UVCO_EABORTED, Resume::later);
    }

    static void onTimer(uv_timer_t* const timer) noexcept {
        auto& self = fromData<TimerBlock>(timer->data);
        ++self.expirations_;
        if (self.state_ == State::pending) {
            self.taken_ = std::exchange(self.expirations_, 0);
            self.finishTick(0, Resume::inlineNow); // 之后不再访问 self
        }
    }

    static void onCancel(void* const context) {
        auto* const self = static_cast<TimerBlock*>(context);
        if (self->state_ == State::pending) self->finishTick(UV_ECANCELED, Resume::inlineNow);
    }

    void finishTick(int const status, Resume const how) noexcept {
        status_ = status;
        state_ = State::done;
        co2::coroutine_handle<> const waiter = waiter_;
        if (how == Resume::later)
            resumeLater(waiter);
        else
            waiter.resume();
    }

    uv_timer_t timer_{};
    std::uint64_t expirations_ = 0;
    std::uint64_t taken_ = 0;
    co2::coroutine_handle<> waiter_;
    int status_ = 0;
    State state_ = State::idle;
    bool suspended_ = false;
    CancelHook cancel_;
};

class TickAwaiter {
  public:
    explicit TickAwaiter(TimerBlock& block) noexcept : block_{&block} {}

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return block_->suspendTick(waiting, co2::detail::stopTokenOf(waiting));
    }

    Result<std::uint64_t> await_resume() noexcept { return block_->resumeTick(); }

  private:
    HandleRef<TimerBlock> block_;
};

} // namespace detail

class Timer : public detail::Handle<detail::TimerBlock> {
  public:
    explicit Timer(Loop& loop) : Handle{new detail::TimerBlock{loop}} {}

    // 启动（或重启）：从调用时刻起 timeout 后首次到期，之后每 repeat 到期一次（0 = 一次性）。清空累计。
    Result<void> start(std::chrono::milliseconds const timeout,
                       std::chrono::milliseconds const repeat = std::chrono::milliseconds{0}) {
        return block().start(detail::toMilliseconds(timeout), detail::toMilliseconds(repeat));
    }

    // 停止；挂起中的 tick() 以 Errc::aborted 结束。清空累计。
    void stop() noexcept { block().stop(); }

    void setRepeat(std::chrono::milliseconds const repeat) noexcept {
        uv_timer_set_repeat(block().raw(), detail::toMilliseconds(repeat));
    }
    std::chrono::milliseconds repeat() const noexcept {
        return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
            uv_timer_get_repeat(block().raw()))};
    }
    // 距下一次到期的时间（未启动为 0）。
    std::chrono::milliseconds dueIn() const noexcept {
        return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
            uv_timer_get_due_in(block().raw()))};
    }
    bool isActive() const noexcept {
        return uv_is_active(reinterpret_cast<uv_handle_t const*>(block().raw())) != 0;
    }

    UVCO_NODISCARD detail::TickAwaiter tick() noexcept { return detail::TickAwaiter{block()}; }
};

} // namespace uvco
