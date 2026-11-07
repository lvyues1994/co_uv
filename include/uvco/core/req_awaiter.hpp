#pragma once

#include <type_traits>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <uv.h>

#include "uvco/core/cancel.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"

// detail::ReqAwaiter<Derived, Req, T>：单次 libuv 请求（uv_fs_t、uv_connect_t、
// uv_write_t、uv_getaddrinfo_t……）的 awaiter 模板（docs/architecture.md §4.1）。
//
//   - 请求体 Req 是 awaiter 的成员；req.data 指向 awaiter。awaiter 在协程帧里、地址
//     稳定，因此 libuv 可以一直持有 &req。
//   - 全部在 loop 线程上发生：不需要原子量（不复用 co2::CallbackAwaitable）。
//   - libuv 回调里调用 complete(req, status)，它记录状态并 resume() 等待者——等待者
//     在 loop 线程上内联恢复。
//
// Derived 需要提供（CRTP）：
//   int start(uv_loop_t* loop);        // 发起请求（回调里调用 complete），返回 uv 状态码
//   Result<T> finish(int status);      // 在 await_resume 中把状态/请求体转换为结果
// 可选：
//   static constexpr bool cancellable = false;  // 默认 true：支持 stop_token 取消
//   void cancel() noexcept;                     // 默认 uv_cancel(&req)
//
// 取消（§4.3）：
//   - await_suspend 时若 stop 已经被请求，不发起请求，直接以 UV_ECANCELED 完成；
//   - 否则发起请求后经 detail::CancelHook 注册 stop_callback：回调可能在任意线程
//     触发，只投递到 loop；在 loop 线程上若请求仍在途则执行 Derived::cancel()；
//   - 被取消的请求照常经由 libuv 回调“正常完成”（通常是 UV_ECANCELED），协程随后
//     走到终点——满足 co2 的销毁契约。uv_cancel 失败（请求已在执行）则等它自然完成；
//   - await_resume 先 disarm（注销 stop_callback 并撤销未执行的投递），之后 awaiter
//     才可以被销毁。
//
// 两种用法：
//   1. 右值：CO2_AWAIT_SET(r, FsStat{loop, path});  awaiter 被移进 co2 的 awaiter 槽，
//      超过内联容量（8 指针）时退化为一次堆分配；
//   2. 帧局部（零分配，§7“大请求存储”）：把 awaiter 声明在 CO2_BEG 的局部列表里，
//      以左值等待：CO2_AWAIT_SET(r, op); operator_co_await() & 返回一个只含指针的
//      引用 awaiter，请求体就住在协程帧里。完成后可以再次等待同一个对象。

namespace uvco {
namespace detail {

template <class Derived, class = void> struct IsCancellable : std::true_type {};
template <class Derived>
struct IsCancellable<Derived, typename std::enable_if<not Derived::cancellable>::type>
    : std::false_type {};

template<class D, class = void> struct RespectsStop : std::true_type {};
template<class D> struct RespectsStop<D, typename std::enable_if<not D::respectStop>::type>
 : std::false_type {};

template <class Awaiter> struct AwaiterRef {
    Awaiter* awaiter;

    bool await_ready() const noexcept { return awaiter->await_ready(); }

    template <class Promise>
    bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return awaiter->await_suspend(waiting);
    }

    decltype(auto) await_resume() { return awaiter->await_resume(); }
};

template <class Derived, class Req, class T> class ReqAwaiter {
  public:
    using result_type = Result<T>;

    ReqAwaiter(ReqAwaiter const&) = delete;
    ReqAwaiter& operator=(ReqAwaiter const&) = delete;
    ReqAwaiter& operator=(ReqAwaiter&&) = delete;

    // 只在发起前移动（右值等待时 awaiter 被移进帧）。
    ReqAwaiter(ReqAwaiter&& other) noexcept : req_(other.req_), loop_{other.loop_} {
        UVCO_CONTRACT_CHECK(other.state_ == State::idle);
    }

    ~ReqAwaiter() {
        // 请求在途时销毁：libuv 仍持有 &req_。
        UVCO_CONTRACT_CHECK(state_ != State::pending);
    }

    Loop& loop() const noexcept { return *loop_; }

    // ---- awaiter 协议 ----

    bool await_ready() const noexcept { return false; }

    template <class Promise>
    bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        UVCO_CONTRACT_CHECK(state_ == State::idle);
        UVCO_CONTRACT_CHECK(loop_->isLoopThread());
        co2::stop_token token = co2::detail::stopTokenOf(waiting);
        stopToken_ = token;
        if (RespectsStop<Derived>::value && token.stop_requested()) {
            status_ = UV_ECANCELED;
            state_ = State::done;
            return false;
        }
        req_.data = this;
        waiter_ = waiting;
        int const status = derived().start(loop_->raw());
        if (status < 0) {
            status_ = status;
            state_ = State::done;
            return false;
        }
        state_ = State::pending;
        registerStop(std::move(token), IsCancellable<Derived>{});
        return true;
    }

    Result<T> await_resume() {
        UVCO_CONTRACT_CHECK(state_ == State::done);
        cancelHook_.disarm();
        state_ = State::idle;
        waiter_ = nullptr;
        return derived().finish(status_);
    }

    // 帧局部用法：以左值等待时借用自己，不复制请求体。
    AwaiterRef<Derived> operator_co_await() & noexcept {
        return AwaiterRef<Derived>{&derived()};
    }

  protected:
    explicit ReqAwaiter(Loop& loop) noexcept : loop_{&loop} {}

    // 在 libuv 回调中调用（loop 线程）。
    static void complete(Req* const req, int const status) {
        auto* const self = static_cast<ReqAwaiter*>(req->data);
        UVCO_CONTRACT_CHECK(self->state_ == State::pending);
        self->status_ = status;
        self->state_ = State::done;
        self->waiter_.resume();
    }

    // 默认取消：uv_cancel 只对 fs / work / getaddrinfo / getnameinfo / random 有效。
    void cancel() noexcept {
        static_cast<void>(uv_cancel(reinterpret_cast<uv_req_t*>(&req_)));
    }

    Derived& derived() noexcept { return static_cast<Derived&>(*this); }

    // 请求是否在途（已发起、回调尚未到来）。
    bool stopRequested() const noexcept { return stopToken_.stop_requested(); }

    bool inFlight() const noexcept { return state_ == State::pending; }

    Req req_{};

  private:
    enum class State : unsigned char { idle, pending, done };

    static void runCancel(void* const context) {
        auto* const self = static_cast<ReqAwaiter*>(context);
        if (self->state_ == State::pending) self->derived().cancel();
    }

    void registerStop(co2::stop_token token, std::true_type) {
        cancelHook_.arm(*loop_, std::move(token), &ReqAwaiter::runCancel, this);
    }

    void registerStop(co2::stop_token, std::false_type) noexcept {}

    Loop* loop_;
    co2::coroutine_handle<> waiter_;
    int status_ = 0;
    State state_ = State::idle;
    CancelHook cancelHook_;
    co2::stop_token stopToken_;
};

} // namespace detail
} // namespace uvco
