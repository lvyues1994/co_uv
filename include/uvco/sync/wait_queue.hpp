#pragma once

#include <memory>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/stop_token.hpp>
#include "uvco/detail/libuv.hpp"

#include "uvco/core/cancel.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"

// detail::WaitQueue / WaitNode：sync 原语（Event、Mutex、Channel）共用的 FIFO 等待队列
// （docs/architecture.md §4.5）。
//
//   - 全部在 loop 线程上使用，不加锁；跨线程调用是契约违规。
//   - 节点（WaitNode）是 awaiter 的成员：awaiter 在 co2 的 awaiter 槽里、地址稳定。
//     节点在队列中时 awaiter 不能被销毁（契约）。
//   - 取消：挂起时若 stop_token 可能停止，才为节点分配一个 CancelHook（不可停止的等待、
//     不挂起的快速路径都零分配）。停止请求经 loop 投递，在 loop 线程上若节点仍在队列中则
//     摘下并以 UV_ECANCELED 恢复。
//   - 恢复策略（§4.5“恢复策略”）：
//       * wake()（由用户代码触发：set / unlock / send / recv / close）一律经
//         loop.schedule() 延后恢复——唤醒方继续执行，不在它的栈上运行等待者；
//       * 取消在 loop 的 async 回调里执行（栈上没有用户帧），就地恢复。
//   - 被唤醒（出队）之后的等待者一定以唤醒时给定的状态完成；之后到达的取消请求不起作用。

namespace uvco {
namespace detail {

class WaitQueue;

struct WaitNode {
    WaitNode() noexcept = default;
    WaitNode(WaitNode const&) = delete;
    WaitNode& operator=(WaitNode const&) = delete;
    // 只允许在挂起前移动（右值 awaiter 被移进 awaiter 槽）。
    WaitNode(WaitNode&& other) noexcept { UVCO_CONTRACT_CHECK(other.queue == nullptr); }
    WaitNode& operator=(WaitNode&&) = delete;

    ~WaitNode() { UVCO_CONTRACT_CHECK(queue == nullptr); }

    bool queued() const noexcept { return queue != nullptr; }

    // await_resume 时调用：注销取消回调。之后节点可以销毁。
    void finish() noexcept {
        UVCO_CONTRACT_CHECK(queue == nullptr);
        if (cancel) cancel->disarm();
    }

    WaitNode* previous = nullptr;
    WaitNode* next = nullptr;
    WaitQueue* queue = nullptr; // 非空 ⇔ 在队列中
    co2::coroutine_handle<> waiter;
    int status = 0;
    std::unique_ptr<CancelHook> cancel;
};

class WaitQueue {
  public:
    explicit WaitQueue(Loop& loop) noexcept : loop_{&loop} {}
    WaitQueue(WaitQueue const&) = delete;
    WaitQueue& operator=(WaitQueue const&) = delete;

    // 带着等待者销毁：等待者永远不会被恢复。
    ~WaitQueue() { UVCO_CONTRACT_CHECK(head_ == nullptr); }

    Loop& loop() const noexcept { return *loop_; }
    bool empty() const noexcept { return head_ == nullptr; }
    WaitNode* front() const noexcept { return head_; }

    void checkThread() const noexcept { UVCO_CONTRACT_CHECK(loop_->isLoopThread()); }

    // 挂起 waiter：入队尾，按需注册取消。
    void enqueue(WaitNode& node, co2::coroutine_handle<> const waiter, co2::stop_token token) {
        checkThread();
        UVCO_CONTRACT_CHECK(node.queue == nullptr);
        node.waiter = waiter;
        node.status = 0;
        node.queue = this;
        node.previous = tail_;
        node.next = nullptr;
        if (tail_ != nullptr)
            tail_->next = &node;
        else
            head_ = &node;
        tail_ = &node;
        if (token.stop_possible()) {
            if (not node.cancel) node.cancel.reset(new CancelHook{});
            node.cancel->arm(*loop_, std::move(token), &WaitQueue::onCancel, &node);
        }
    }

    // 摘下队首（不唤醒）；调用方填好结果后调用 wake()。
    WaitNode& pop() noexcept {
        UVCO_CONTRACT_CHECK(head_ != nullptr);
        WaitNode& node = *head_;
        unlink(node);
        return node;
    }

    // 以 status 延后恢复一个已摘下的节点。
    void wake(WaitNode& node, int const status) noexcept {
        UVCO_CONTRACT_CHECK(node.queue == nullptr);
        node.status = status;
        loop_->schedule(node.waiter);
    }

    void wakeFront(int const status) noexcept { wake(pop(), status); }

    void wakeAll(int const status) noexcept {
        while (head_ != nullptr)
            wakeFront(status);
    }

  private:
    void unlink(WaitNode& node) noexcept {
        UVCO_CONTRACT_CHECK(node.queue == this);
        if (node.previous != nullptr)
            node.previous->next = node.next;
        else
            head_ = node.next;
        if (node.next != nullptr)
            node.next->previous = node.previous;
        else
            tail_ = node.previous;
        node.previous = nullptr;
        node.next = nullptr;
        node.queue = nullptr;
    }

    // loop 线程（async 回调里）：仍在队列中则以 UV_ECANCELED 就地恢复。
    static void onCancel(void* const context) {
        auto& node = *static_cast<WaitNode*>(context);
        if (node.queue == nullptr) return; // 已被唤醒
        node.queue->unlink(node);
        node.status = UV_ECANCELED;
        node.waiter.resume(); // 之后不再访问 node
    }

    Loop* loop_;
    WaitNode* head_ = nullptr;
    WaitNode* tail_ = nullptr;
};

} // namespace detail
} // namespace uvco
