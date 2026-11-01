#pragma once

#include <utility>

#include <co2/coroutine_handle.hpp>
#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"

// libuv 句柄的生命周期（docs/architecture.md §4.2），供 Timer、Stream、Udp 等复用。
//
// uv_close 是异步的，而析构函数无法等待。因此：
//
//   - detail::HandleBlock：堆上的控制块，内含 uv_xxx_t 和各操作的状态。handle->data 指向
//     控制块。它在 close 回调触发 **且** 没有在途操作（opBegin/opEnd 计数归零）时
//     delete 自己——在途操作的等待者被 close 以 UV_ECANCELED 结束、但尚未恢复时，
//     控制块仍然有效，await_resume 可以安全访问它。
//   - detail::Handle<Block>：用户可见句柄（Timer、Tcp……）的基类，只持有控制块指针，
//     只可移动。析构时若尚未关闭则 uv_close（不等待）；此时有挂起的操作是契约违规。
//   - close()：返回一个 awaiter，CO2_AWAIT(h.close()) 等到 close 回调。调用 close() 后
//     句柄立即变空（控制块交给 awaiter）；对空句柄 close() 立即完成。close 时挂起中
//     的操作（例如另一个协程里的 tick()）以 UV_ECANCELED 结束（经 loop 调度恢复）。
//     返回的 awaiter 若从未被等待，析构时照样发起关闭。
//
// 关闭只发起 uv_close，真正释放发生在之后的 loop 迭代里；Loop 析构时会再跑一次
// UV_RUN_NOWAIT，因此“析构句柄后紧接着析构 Loop”也是合法的。
//
// 控制块也可以在用户句柄仍持有它时被内部关闭（closeInternally()，例如取消 connect）：
// 此后操作立即失败，句柄的 close()/析构只是放弃控制块（必要时等到 close 回调）。
//
// 派生控制块需要：uv_xxx_init 之后调用 bindHandle()，并实现
//   uv_handle_t* uvHandle() noexcept override;
//   void abortPending() noexcept override;   // 关闭时：以 UV_ECANCELED 结束挂起的操作
// 并在操作挂起/恢复时调用 opBegin()/opEnd()。

namespace uvco {
namespace detail {

class HandleBlock {
  public:
    HandleBlock(HandleBlock const&) = delete;
    HandleBlock& operator=(HandleBlock const&) = delete;

    Loop& loop() const noexcept { return *loop_; }
    bool closing() const noexcept { return closing_; }
    bool hasPendingOps() const noexcept { return outstanding_ != 0; }

    // loop 线程：用户句柄放弃控制块（close() 或析构）。尚未关闭则发起关闭。
    // 返回 true 表示 closeWaiter 会在 close 回调里恢复；false 表示已经关闭完毕
    // （例如控制块此前已被内部关闭），调用者不必挂起。closeWaiter 可为空。只能调用一次。
    bool release(co2::coroutine_handle<> const closeWaiter) noexcept {
        UVCO_CONTRACT_CHECK(loop_->isLoopThread());
        UVCO_CONTRACT_CHECK(owned_);
        owned_ = false;
        if (closed_) {
            destroyIfDone();
            return false;
        }
        closeWaiter_ = closeWaiter;
        closeInternally();
        return static_cast<bool>(closeWaiter);
    }

    // loop 线程：在用户句柄仍持有控制块时发起关闭（例如取消 connect：libuv 无法取消
    // 连接请求，只能关闭 socket）。之后句柄上的操作立即失败；重复调用无效果。
    void closeInternally() noexcept {
        UVCO_CONTRACT_CHECK(loop_->isLoopThread());
        if (closing_) return;
        closing_ = true;
        uv_close(uvHandle(), &HandleBlock::onClose);
        abortPending();
    }

  protected:
    explicit HandleBlock(Loop& loop) noexcept : loop_{&loop} {
        UVCO_CONTRACT_CHECK(loop.isLoopThread());
    }
    virtual ~HandleBlock() = default;

    virtual uv_handle_t* uvHandle() noexcept = 0;
    virtual void abortPending() noexcept = 0;

    // 派生类在 uv_xxx_init 之后调用：handle->data 指向控制块（以 HandleBlock* 存储）。
    void bindHandle() noexcept { uvHandle()->data = static_cast<HandleBlock*>(this); }

    // 在 libuv 回调里由 handle->data 取回派生控制块。
    template <class Derived> static Derived& fromData(void* const data) noexcept {
        return static_cast<Derived&>(*static_cast<HandleBlock*>(data));
    }

    // 操作挂起时 opBegin()；恢复（await_resume）时最后一步 opEnd()——之后控制块可能已被释放。
    void opBegin() noexcept { ++outstanding_; }
    void opEnd() noexcept {
        UVCO_CONTRACT_CHECK(outstanding_ != 0);
        --outstanding_;
        destroyIfDone();
    }

    // 结束一个操作：正在 libuv 回调里时直接内联恢复；在用户调用栈里（close()、stop()……）
    // 时经 loop 调度，避免在调用者的栈帧里恢复别的协程。
    void resumeLater(co2::coroutine_handle<> const waiter) noexcept { loop_->schedule(waiter); }

  private:
    static void onClose(uv_handle_t* const handle) noexcept {
        HandleBlock* const self = &fromData<HandleBlock>(handle->data);
        co2::coroutine_handle<> const waiter = self->closeWaiter_;
        self->closed_ = true;
        self->destroyIfDone(); // 之后不再访问 self
        if (waiter) waiter.resume();
    }

    void destroyIfDone() noexcept {
        if (closed_ && outstanding_ == 0 && not owned_) delete this;
    }

    Loop* loop_;
    co2::coroutine_handle<> closeWaiter_;
    unsigned outstanding_ = 0;
    bool owned_ = true; // 用户句柄（Handle<Block>）仍持有控制块
    bool closing_ = false;
    bool closed_ = false;
};

// close() 的 awaiter：持有控制块指针（指针大小，放进 co2 的内联槽）。
class CloseAwaiter {
  public:
    explicit CloseAwaiter(HandleBlock* const block) noexcept : block_{block} {}
    CloseAwaiter(CloseAwaiter&& other) noexcept : block_{std::exchange(other.block_, nullptr)} {}
    CloseAwaiter(CloseAwaiter const&) = delete;
    CloseAwaiter& operator=(CloseAwaiter const&) = delete;
    CloseAwaiter& operator=(CloseAwaiter&&) = delete;

    ~CloseAwaiter() {
        if (block_ != nullptr) static_cast<void>(block_->release({})); // 从未被等待：照样关闭
    }

    bool await_ready() const noexcept { return block_ == nullptr; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return std::exchange(block_, nullptr)->release(waiting);
    }

    void await_resume() const noexcept {}

  private:
    HandleBlock* block_;
};

template <class Block> class Handle {
  public:
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;

    Handle(Handle&& other) noexcept : block_{std::exchange(other.block_, nullptr)} {}

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            block_ = std::exchange(other.block_, nullptr);
        }
        return *this;
    }

    ~Handle() { reset(); }

    // 是否仍持有一个未关闭的句柄（被移走或 close() 之后为 false）。
    bool isOpen() const noexcept { return block_ != nullptr; }
    explicit operator bool() const noexcept { return isOpen(); }

    Loop& loop() const noexcept {
        UVCO_CONTRACT_CHECK(block_ != nullptr);
        return block_->loop();
    }

    // CO2_AWAIT(h.close())：发起 uv_close 并等到 close 回调。调用后句柄变空。
    UVCO_NODISCARD CloseAwaiter close() noexcept {
        if (block_ != nullptr) UVCO_CONTRACT_CHECK(block_->loop().isLoopThread());
        return CloseAwaiter{std::exchange(block_, nullptr)};
    }

  protected:
    Handle() noexcept : block_{nullptr} {}
    explicit Handle(Block* const block) noexcept : block_{block} {}

    // 在 loop 线程上取得控制块；空句柄上操作是契约违规。
    Block& block() const noexcept {
        UVCO_CONTRACT_CHECK(block_ != nullptr);
        UVCO_CONTRACT_CHECK(block_->loop().isLoopThread());
        return *block_;
    }

  private:
    void reset() noexcept {
        Block* const block = std::exchange(block_, nullptr);
        if (block == nullptr) return;
        UVCO_CONTRACT_CHECK(block->loop().isLoopThread());
        // §4.2：句柄上有挂起中的操作时析构是契约违规（等待者的帧还引用着它）。
        if (block->hasPendingOps())
            UVCO_CONTRACT_FAIL("uvco handle destroyed while an operation is pending");
        static_cast<void>(block->release({}));
    }

    Block* block_;
};

} // namespace detail
} // namespace uvco
