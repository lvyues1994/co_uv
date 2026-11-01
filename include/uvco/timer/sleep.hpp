#pragma once

#include <chrono>
#include <cstdint>

#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/req_awaiter.hpp"
#include "uvco/core/result.hpp"
#include "uvco/timer/timer.hpp"

// sleep(loop, duration) -> Result<void>：一次性等待（docs/architecture.md §4.4）。
//
//   CO2_AWAIT_SET(r, uvco::sleep(loop, 50ms));     // 右值：一次堆分配（awaiter 超出内联槽）
//
//   uvco::Sleep nap{loop, 50ms};                    // 帧局部（CO2_BEG 局部列表）：零分配，
//   CO2_AWAIT_SET(r, nap);                          // 可重复等待
//
// 计时从等待时刻起算（启动前 uv_update_time），保证至少等待 duration（毫秒粒度）。
// 取消：stop_token 被请求 → UV_ECANCELED（等待前已请求则不启动计时器）。
//
// 实现：基于 detail::ReqAwaiter，uv_timer_t 直接放在 awaiter 里。每次等待 init 一个
// 计时器，到期或取消后先 uv_close，close 回调里才恢复等待者——等待者恢复时 libuv 已
// 不再引用 awaiter 内的句柄，awaiter 可以随帧销毁。代价是每次 sleep 多一次 close
// 的 loop 迭代；需要高频定时请用 Timer。

namespace uvco {

class Sleep : public detail::ReqAwaiter<Sleep, uv_timer_t, void> {
    using Base = detail::ReqAwaiter<Sleep, uv_timer_t, void>;

  public:
    Sleep(Loop& loop, std::chrono::milliseconds const duration) noexcept
        : Base{loop}, duration_{detail::toMilliseconds(duration)} {}
    Sleep(Sleep&&) = default;

    void setDuration(std::chrono::milliseconds const duration) noexcept {
        duration_ = detail::toMilliseconds(duration);
    }

    // ---- ReqAwaiter 钩子 ----

    int start(uv_loop_t* const loop) noexcept {
        int status = uv_timer_init(loop, &req_);
        UVCO_CONTRACT_CHECK(status == 0);
        req_.data = static_cast<Base*>(this);
        closing_ = false;
        closeStatus_ = 0;
        uv_update_time(loop); // 从调用时刻起算，而不是从本次迭代开始时缓存的 uv_now 起算
        status = uv_timer_start(&req_, &Sleep::onTimer, duration_, 0);
        UVCO_CONTRACT_CHECK(status == 0); // 只会因回调为空失败
        return 0;
    }

    Result<void> finish(int const status) noexcept { return fromStatus(status); }

    void cancel() noexcept { closeWith(UV_ECANCELED); }

  private:
    static Sleep& self(uv_timer_t* const timer) noexcept {
        return static_cast<Sleep&>(*static_cast<Base*>(timer->data));
    }

    static void onTimer(uv_timer_t* const timer) noexcept { self(timer).closeWith(0); }

    void closeWith(int const status) noexcept {
        if (closing_) return;
        closing_ = true;
        closeStatus_ = status;
        uv_close(reinterpret_cast<uv_handle_t*>(&req_), &Sleep::onClosed);
    }

    static void onClosed(uv_handle_t* const handle) noexcept {
        auto* const timer = reinterpret_cast<uv_timer_t*>(handle);
        complete(timer, self(timer).closeStatus_);
    }

    std::uint64_t duration_;
    int closeStatus_ = 0;
    bool closing_ = false;
};

UVCO_NODISCARD inline Sleep sleep(Loop& loop, std::chrono::milliseconds const duration) noexcept {
    return Sleep{loop, duration};
}

} // namespace uvco
