#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include <co2/stop_token.hpp>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"

// detail::CancelHook：把 stop_token 的停止请求转交给 loop 线程（docs/architecture.md §4.3）。
//
//   - arm(loop, token, fn, ctx)（loop 线程）：注册 stop_callback。回调可能在任意线程触发，
//     它只把一个侵入式节点 post 到 loop（零分配、不碰 libuv）；节点在 loop 线程上调用
//     fn(ctx)。即使请求来自 loop 线程本身，也延后到下一次 async 回调执行，避免在
//     request_stop() 的调用栈里重入 libuv。
//   - disarm()（loop 线程）：先析构 stop_callback（若回调正在别的线程执行则等它返回），
//     再撤销尚未执行的节点。之后 fn 不会再被调用，hook 可以被销毁或再次 arm。
//   - fn 自己判断操作是否仍在途：节点执行时操作可能刚好已经完成。
//
// 被 ReqAwaiter（请求）与各句柄的控制块（timer tick、stream read……）复用。

namespace uvco {
namespace detail {

class CancelHook {
  public:
    using Fn = void (*)(void* context);

    CancelHook() noexcept = default;
    CancelHook(CancelHook const&) = delete;
    CancelHook& operator=(CancelHook const&) = delete;

    ~CancelHook() { UVCO_CONTRACT_CHECK(not armed_); }

    bool armed() const noexcept { return armed_; }

    // token 不可能停止时什么也不做。若 stop 已被请求，fn 仍会（异步地）被调用一次。
    void arm(Loop& loop, co2::stop_token token, Fn const fn, void* const context) {
        UVCO_CONTRACT_CHECK(not armed_);
        UVCO_CONTRACT_CHECK(loop.isLoopThread());
        if (not token.stop_possible()) return;
        loop_ = &loop;
        fn_ = fn;
        context_ = context;
        node_.invoke = &CancelHook::run;
        node_.self = this;
        ::new (static_cast<void*>(&storage_)) StopCallback{std::move(token), StopFn{this}};
        armed_ = true;
    }

    void disarm() noexcept {
        if (not armed_) return;
        UVCO_CONTRACT_CHECK(loop_->isLoopThread());
        reinterpret_cast<StopCallback*>(&storage_)->~StopCallback();
        armed_ = false;
        loop_->unpost(&node_);
    }

  private:
    struct StopFn {
        CancelHook* self;
        // 任意线程：只投递。
        void operator()() const noexcept { self->loop_->post(&self->node_); }
    };
    using StopCallback = co2::stop_callback<StopFn>;

    struct Node : LoopCallback {
        CancelHook* self = nullptr;
    };

    static void run(LoopCallback* const node) noexcept {
        auto* const self = static_cast<Node*>(node)->self;
        self->fn_(self->context_);
    }

    Loop* loop_ = nullptr;
    Fn fn_ = nullptr;
    void* context_ = nullptr;
    bool armed_ = false;
    Node node_;
    typename std::aligned_storage<sizeof(StopCallback), alignof(StopCallback)>::type storage_;
};

} // namespace detail
} // namespace uvco
