#pragma once

#include <chrono>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <co2/coroutine_handle.hpp>

#include "uvco/core/error.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/sync/when_any.hpp"
#include "uvco/timer/sleep.hpp"

// timeout / runUntil：基于 whenAny 的“主操作 + 停止条件”（docs/architecture.md §4.5）。
//
//   CO2_AWAIT_SET(r, uvco::timeout(loop, 100ms, conn.read(buf)));   // Result<size_t>
//   CO2_AWAIT_SET(r, uvco::runUntil(fetch(), shutdown.wait()));      // Result<T>
//
// 主操作是任意 awaitable；结果类型 Result<S>，S 为主操作的成功值（Result<U> 解包为 U，
// 普通值 V 原样）。
//
// timeout(loop, d, op) = whenAny(op, sleep(loop, d))：
//   - op 先成功 → 它的值；计时器先到 → UV_ETIMEDOUT（op 被请求停止并被等待结束）。
//   - op 的错误 / 异常按 whenAny 的优先级传出（异常 > 非取消错误 > 先结束者）。
//   - 父取消 → UV_ECANCELED（不是超时）。op 忽略取消时，timeout 一直等到它结束。
//   - op 先启动；op 同步完成时计时器根本不会启动。
//
// runUntil(op, stops...) = whenAny(stops..., op)：任一 stop 先**成功**结束 → op 被取消，
// 结果 UV_ECANCELED；stop 的成功值被丢弃，stop 的非取消错误照常传出。stops 先启动：
// 已经满足的 stop 让 op 根本不启动。
//
// 返回 awaitable（不是 Task），结果映射不额外占帧。

namespace uvco {
namespace detail {

template <std::size_t Main, class Inner> class RaceAwaiter {
    using Value = typename Inner::Value;

  public:
    using Success = typename Value::template Element<Main>;

    RaceAwaiter(Inner inner, Errc const stopError) : inner_{std::move(inner)}, stopError_{stopError} {}

    bool await_ready() const noexcept { return inner_.await_ready(); }

    template <class Parent> bool await_suspend(co2::coroutine_handle<Parent> const awaiting) {
        return inner_.await_suspend(awaiting);
    }

    Result<Success> await_resume() {
        Result<Value> r = inner_.await_resume();
        if (not r) return Result<Success>{r.error()};
        if (r->index() != Main) return Result<Success>{stopError_};
        return extract(*r, std::is_void<Success>{});
    }

  private:
    static Result<Success> extract(Value& value, std::false_type) {
        return Result<Success>{std::move(value).template get<Main>()};
    }
    static Result<Success> extract(Value&, std::true_type) { return Result<Success>{}; }

    Inner inner_;
    Errc stopError_;
};

} // namespace detail

template <class A>
UVCO_NODISCARD detail::RaceAwaiter<
    0U, detail::WhenAnyTuple<detail::BranchValue<A>, Result<void>>>
timeout(Loop& loop, std::chrono::milliseconds const duration, A op) {
    return {whenAny(std::move(op), sleep(loop, duration)), Errc::timed_out};
}

template <class A, class... Stops>
UVCO_NODISCARD detail::RaceAwaiter<
    sizeof...(Stops),
    detail::WhenAnyTuple<detail::BranchValue<Stops>..., detail::BranchValue<A>>>
runUntil(A op, Stops... stops) {
    static_assert(sizeof...(Stops) != 0U, "uvco::runUntil needs at least one stop");
    return {whenAny(std::move(stops)..., std::move(op)), Errc::canceled};
}

} // namespace uvco
