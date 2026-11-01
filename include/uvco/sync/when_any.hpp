#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include <co2/coroutine_handle.hpp>
#include <co2/detail/late_init.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <co2/task.hpp>

#include "uvco/core/config.hpp"

// whenAny：并发启动一组同类型 Task，**第一个完成者**胜出（docs/architecture.md §4.5）。
//
//   CO2_AWAIT_SET(r, uvco::whenAny(a(), b(), c()));   // uvco::WhenAnyResult<T>
//   r.index;   // 胜出者下标（参数顺序）
//   r.value;   // 胜出者的结果（T 为 void 时没有该成员）
//
//   - 内部 stop_source 链到父 stop_token（父取消会转发给全部分支）。第一个完成的分支
//     立即对其余分支 request_stop()。
//   - **等全部分支完成后**才恢复等待者（co2 销毁契约：已启动的 Task 必须走到终点）；
//     忽略取消的分支会把 whenAny 拖到它自己结束为止。
//   - “完成”包括以异常结束：胜出者的异常在这里重抛；落败者的结果与异常都被丢弃。
//   - 全部分支按参数顺序启动（即使启动途中已有胜出者——之后启动的分支会看到 stop 已
//     请求）。分支数为 0 是契约违规。
//   - 分支可以 scheduleOn 到别的线程；与 co2::whenAll 一样，等待者在最后一个完成的
//     分支所在线程上恢复。
//   - 只支持同类型分支（C++14 没有 variant）；异构分支请先包装成同一类型
//     （例如都返回 Result<X>）。
//
// 实现与 co2::whenAll 同构：awaiter 本身是操作状态，每个分支一个内嵌的手写续体帧，
// 计数初值 N+1，启动方最后放下自己那份。

namespace uvco {

template <class T> struct WhenAnyResult {
    std::size_t index;
    T value;
};

template <> struct WhenAnyResult<void> {
    std::size_t index;
};

namespace detail {

struct WhenAnyState {
    static constexpr std::size_t noWinner = ~std::size_t{0};

    WhenAnyState() = default;
    WhenAnyState(WhenAnyState&& other) noexcept : source{std::move(other.source)} {
        UVCO_CONTRACT_CHECK(not other.awaiting);
    }
    WhenAnyState(WhenAnyState const&) = delete;
    WhenAnyState& operator=(WhenAnyState const&) = delete;
    WhenAnyState& operator=(WhenAnyState&&) = delete;

    void begin(co2::coroutine_handle<> const awaiting_, co2::stop_token parent,
               std::size_t const count) {
        awaiting = awaiting_;
        remaining.store(count + 1U, std::memory_order_relaxed);
        if (parent.stop_possible())
            forwarding.emplace(std::move(parent), co2::detail::ForwardStop{&source});
    }

    // 分支完成：第一个到达者成为胜出者并请求其余分支停止；最后到达者拿到等待者。
    // request_stop() 在递减计数之前：返回前等待者不可能被恢复、销毁本状态。
    co2::coroutine_handle<> childCompleted(std::size_t const index) noexcept {
        auto expected = noWinner;
        if (winner.compare_exchange_strong(expected, index, std::memory_order_acq_rel))
            source.request_stop();
        if (remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U) return awaiting;
        return nullptr;
    }

    bool finishStart() noexcept {
        return remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U;
    }

    std::size_t winnerIndex() const noexcept { return winner.load(std::memory_order_acquire); }

    std::atomic<std::size_t> remaining{0U};
    std::atomic<std::size_t> winner{noWinner};
    co2::coroutine_handle<> awaiting;
    co2::stop_source source;
    co2::detail::LateInit<co2::stop_callback<co2::detail::ForwardStop>> forwarding;
};

// 每个分支的续体帧：标准布局，首成员是帧头。
struct WhenAnyChild {
    co2::detail::FrameHeader header;
    WhenAnyState* state;
    std::size_t index;

    co2::coroutine_handle<> handle() noexcept {
        return co2::coroutine_handle<>::from_address(&header);
    }

    static co2::detail::FrameHeader* complete(co2::detail::FrameHeader* const header) {
        static_assert(std::is_standard_layout<WhenAnyChild>::value,
                      "the completion frame must stay standard-layout");
        auto* const self = reinterpret_cast<WhenAnyChild*>(header);
        auto const next = self->state->childCompleted(self->index);
        return next ? co2::detail::HandleAccess::header(next) : nullptr;
    }
};

template <class T, class Tasks, class Children> class WhenAnyAwaiter {
  public:
    WhenAnyAwaiter(Tasks tasks, Children children)
        : tasks_(std::move(tasks)), children_(std::move(children)) {
        UVCO_CONTRACT_CHECK(tasks_.size() != 0U); // 没有分支就没有胜出者
        UVCO_CONTRACT_CHECK(children_.size() == tasks_.size());
    }

    WhenAnyAwaiter(WhenAnyAwaiter&&) = default;
    WhenAnyAwaiter(WhenAnyAwaiter const&) = delete;
    WhenAnyAwaiter& operator=(WhenAnyAwaiter const&) = delete;
    WhenAnyAwaiter& operator=(WhenAnyAwaiter&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <class Parent> bool await_suspend(co2::coroutine_handle<Parent> const awaiting) {
        state_.begin(awaiting, co2::detail::stopTokenOf(awaiting), tasks_.size());
        for (std::size_t index = 0; index != tasks_.size(); ++index) {
            auto& task = tasks_[index];
            UVCO_CONTRACT_CHECK(static_cast<bool>(task));
            children_[index] = WhenAnyChild{
                co2::detail::FrameHeader{&co2::detail::noopDestroy, &WhenAnyChild::complete,
                                         false, false},
                &state_, index};
            co2::detail::TaskAccess::arm(task, children_[index].handle(), state_.source.get_token())
                .resume();
        }
        return not state_.finishStart();
    }

    WhenAnyResult<T> await_resume() {
        std::size_t const index = state_.winnerIndex();
        UVCO_CONTRACT_CHECK(index != WhenAnyState::noWinner);
        auto& task = tasks_[index];
        if (co2::detail::TaskAccess::hasError(task)) co2::detail::TaskAccess::rethrowError(task);
        return take(index, task, std::is_void<T>{});
    }

  private:
    static WhenAnyResult<T> take(std::size_t const index, co2::Task<T>& task, std::false_type) {
        return WhenAnyResult<T>{index, co2::detail::TaskAccess::takeResult(task)};
    }
    static WhenAnyResult<T> take(std::size_t const index, co2::Task<T>& task, std::true_type) {
        co2::detail::TaskAccess::takeResult(task);
        return WhenAnyResult<T>{index};
    }

    Tasks tasks_;
    WhenAnyState state_;
    Children children_;
};

template <class...> struct AllSame : std::true_type {};
template <class T, class U, class... Rest>
struct AllSame<T, U, Rest...>
    : std::integral_constant<bool, std::is_same<T, U>::value && AllSame<T, Rest...>::value> {};

template <class T, std::size_t N>
using WhenAnyTuple = WhenAnyAwaiter<T, std::array<co2::Task<T>, N>, std::array<WhenAnyChild, N>>;

template <class T>
using WhenAnyRange = WhenAnyAwaiter<T, std::vector<co2::Task<T>>, std::vector<WhenAnyChild>>;

} // namespace detail

// 同类型的若干 Task。
template <class T, class... U>
detail::WhenAnyTuple<T, 1U + sizeof...(U)> whenAny(co2::Task<T> first, co2::Task<U>... rest) {
    static_assert(detail::AllSame<T, U...>::value,
                  "uvco::whenAny requires all branches to have the same result type");
    return detail::WhenAnyTuple<T, 1U + sizeof...(U)>{
        std::array<co2::Task<T>, 1U + sizeof...(U)>{{std::move(first), std::move(rest)...}},
        std::array<detail::WhenAnyChild, 1U + sizeof...(U)>{}};
}

// 一组 Task（非空）。
template <class T> detail::WhenAnyRange<T> whenAny(std::vector<co2::Task<T>> tasks) {
    std::size_t const count = tasks.size();
    return detail::WhenAnyRange<T>{std::move(tasks), std::vector<detail::WhenAnyChild>(count)};
}

} // namespace uvco
