#pragma once

#include <array>
#include <chrono>
#include <utility>

#include <co2/coroutine.hpp>
#include <co2/coroutine_handle.hpp>
#include <co2/task.hpp>

#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/sync/when_any.hpp"
#include "uvco/timer/sleep.hpp"

// timeout(loop, duration, task)：给一个返回 Result<T> 的 Task 加时限（docs/architecture.md §4.5）。
//
//   CO2_AWAIT_SET(r, uvco::timeout(loop, 100ms, fetch()));   // Result<T>
//   if (not r && r.error() == uvco::Errc::timed_out) ...
//
//   - 基于 whenAny(task, 计时分支)：先完成者胜出，另一方被请求停止；**等两边都结束**
//     才返回。task 忽略取消时，超时后 timeout 会一直等到它结束（结果仍是 UV_ETIMEDOUT）。
//   - task 胜出 → 它的结果（包括它自己的错误与异常）；计时分支胜出 → UV_ETIMEDOUT。
//   - 父 stop_token 被请求：两边都被取消；计时分支先结束时返回 UV_ECANCELED（不是超时）。
//   - 只接受 Task<Result<T>>（uvco 的可失败操作都返回 Result）。
//   - 返回的是 awaitable（不是 Task）：结果映射不额外占一个协程帧；计时分支是一个
//     帧内放 Sleep 的小 Task（一次帧分配）。

namespace uvco {
namespace detail {

template <class T>
auto timeoutBranch(Loop& loop, std::chrono::milliseconds duration)
    CO2_BEG(co2::Task<Result<T>>, (loop, duration), Sleep nap{loop, duration};
            Result<void> slept;) {
    CO2_AWAIT_SET(slept, nap);
    if (slept) {
        CO2_RETURN(Result<T>{Errc::timed_out});
    } else {
        CO2_RETURN(Result<T>{slept.error()});
    }
}
CO2_END

template <class T> class TimeoutAwaiter {
    using Inner = WhenAnyTuple<Result<T>, 2U>;

  public:
    explicit TimeoutAwaiter(Inner inner) : inner_{std::move(inner)} {}

    bool await_ready() const noexcept { return inner_.await_ready(); }

    template <class Parent> bool await_suspend(co2::coroutine_handle<Parent> const awaiting) {
        return inner_.await_suspend(awaiting);
    }

    Result<T> await_resume() { return std::move(inner_.await_resume().value); }

  private:
    Inner inner_;
};

} // namespace detail

template <class T>
detail::TimeoutAwaiter<T> timeout(Loop& loop, std::chrono::milliseconds const duration,
                                  co2::Task<Result<T>> task) {
    return detail::TimeoutAwaiter<T>{
        whenAny(std::move(task), detail::timeoutBranch<T>(loop, duration))};
}

} // namespace uvco
