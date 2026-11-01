#pragma once

#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <co2/coroutine_handle.hpp>
#include <co2/scheduler.hpp>
#include <uv.h>

#include "uvco/core/config.hpp"

// Loop：持有一个 uv_loop_t，并实现 co2::Scheduler。
//
// 线程模型（docs/architecture.md §3）：
//   - Loop 绑定构造它的线程（“loop 线程”），每个线程最多一个 Loop；
//   - libuv 的一切调用只能发生在 loop 线程；违反即契约违规；
//   - schedule() 可在任意线程调用：
//       * loop 线程：放进本地队列，由一个 uv_idle 在下一次迭代排空；
//       * 其他线程：放进互斥量保护的远程队列，并 uv_async_send 唤醒 loop。
//   - 排空时先把队列整体换出再逐个 resume：resume 过程中新排队的协程留到下一批，
//     不会在 libuv 回调里无限递归，也让 I/O 回调有机会穿插执行。
//
// 跨线程投递除了协程句柄，还有 detail::LoopCallback 节点（侵入式、零分配），用于把
// stop_callback 触发的取消请求转交给 loop 线程（§4.3）。
//
// 所有权：用户持有 Loop（不可复制、不可移动）；run(loop, task) 只负责驱动。
// 析构前置条件：没有 run() 正在进行、没有排队的工作、没有用户句柄仍然打开。

namespace uvco {

class Loop;

namespace detail {

// 投递到 loop 线程执行的侵入式节点。节点由投递方持有；在它被执行或被 unpost()
// 之前不能销毁。
struct LoopCallback {
    void (*invoke)(LoopCallback*) noexcept = nullptr;
    LoopCallback* previous = nullptr;
    LoopCallback* next = nullptr;
    bool queued = false;
};

inline Loop*& currentLoopSlot() noexcept {
    thread_local Loop* loop = nullptr;
    return loop;
}

} // namespace detail

class Loop final : public co2::Scheduler {
  public:
    Loop() : owner_{std::this_thread::get_id()} {
        UVCO_CONTRACT_CHECK(detail::currentLoopSlot() == nullptr); // 每线程至多一个 Loop
        int status = uv_loop_init(&loop_);
        UVCO_CONTRACT_CHECK(status == 0);
        loop_.data = this;

        status = uv_async_init(&loop_, &async_, &Loop::onAsync);
        UVCO_CONTRACT_CHECK(status == 0);
        async_.data = this;
        // async 只在 run() 期间保持引用；平时不阻止 uv_run 返回。
        uv_unref(reinterpret_cast<uv_handle_t*>(&async_));

        status = uv_idle_init(&loop_, &idle_);
        UVCO_CONTRACT_CHECK(status == 0);
        idle_.data = this;

        detail::currentLoopSlot() = this;
    }

    Loop(Loop const&) = delete;
    Loop& operator=(Loop const&) = delete;

    ~Loop() override {
        UVCO_CONTRACT_CHECK(isLoopThread());
        UVCO_CONTRACT_CHECK(not running_);
        {
            std::lock_guard<std::mutex> lock{mutex_};
            // 与 co2::ManualExecutor 一致：带着排队工作销毁是契约违规。
            UVCO_CONTRACT_CHECK(remote_.empty() && callbacks_ == nullptr);
        }
        UVCO_CONTRACT_CHECK(local_.empty());

        uv_close(reinterpret_cast<uv_handle_t*>(&async_), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&idle_), nullptr);
        // close 回调在同一次迭代末尾执行。
        uv_run(&loop_, UV_RUN_NOWAIT);
        int const status = uv_loop_close(&loop_);
        if (status == UV_EBUSY) UVCO_CONTRACT_FAIL("uvco::Loop destroyed with open handles or pending requests");
        UVCO_CONTRACT_CHECK(status == 0);
        detail::currentLoopSlot() = nullptr;
    }

    // 当前线程的 Loop（没有则为空）。
    static Loop* current() noexcept { return detail::currentLoopSlot(); }

    uv_loop_t* raw() noexcept { return &loop_; }
    uv_loop_t const* raw() const noexcept { return &loop_; }

    // Loop 从 uv_loop_t 反查（loop->data 指向 Loop）。
    static Loop& from(uv_loop_t* const loop) noexcept {
        UVCO_CONTRACT_CHECK(loop != nullptr && loop->data != nullptr);
        return *static_cast<Loop*>(loop->data);
    }

    bool isLoopThread() const noexcept { return std::this_thread::get_id() == owner_; }

    // 是否有 run() 正在驱动本 loop。
    bool isRunning() const noexcept { return running_; }

    // co2::Scheduler：任意线程可调用。
    void schedule(co2::coroutine_handle<> const coroutine) noexcept override {
        UVCO_CONTRACT_CHECK(static_cast<bool>(coroutine));
        if (isLoopThread()) {
            if (local_.empty()) uv_idle_start(&idle_, &Loop::onIdle);
            local_.push_back(coroutine);
            return;
        }
        std::lock_guard<std::mutex> lock{mutex_};
        remote_.push_back(coroutine);
        // 在锁内发送：一旦解锁，loop 线程就可能排空队列、完成 run() 并销毁 Loop。
        uv_async_send(&async_);
    }

    // 任意线程：把节点投递到 loop 线程执行。已在队列中则不重复投递。
    void post(detail::LoopCallback* const callback) noexcept {
        UVCO_CONTRACT_CHECK(callback != nullptr && callback->invoke != nullptr);
        std::lock_guard<std::mutex> lock{mutex_};
        if (callback->queued) return;
        callback->previous = nullptr;
        callback->next = callbacks_;
        if (callbacks_ != nullptr) callbacks_->previous = callback;
        callbacks_ = callback;
        callback->queued = true;
        uv_async_send(&async_);
    }

    // loop 线程：撤销尚未执行的投递（节点即将销毁时调用）。返回是否撤销了一个投递。
    bool unpost(detail::LoopCallback* const callback) noexcept {
        UVCO_CONTRACT_CHECK(isLoopThread());
        std::lock_guard<std::mutex> lock{mutex_};
        if (not callback->queued) return false;
        unlink(callback);
        return true;
    }

    // ---- 供 run() 使用 ----

    // 以 UV_RUN_DEFAULT 运行直到 stop() 被调用（在 loop 线程）。run() 期间 async 句柄
    // 保持引用，因此协程跑在别的线程上时 uv_run 阻塞等待，而不是空转返回。
    void runUntilStopped(bool const& stopped) {
        UVCO_CONTRACT_CHECK(isLoopThread());
        UVCO_CONTRACT_CHECK(not running_); // uv_run 不可重入
        running_ = true;
        uv_ref(reinterpret_cast<uv_handle_t*>(&async_));
        while (not stopped)
            uv_run(&loop_, UV_RUN_DEFAULT);
        uv_unref(reinterpret_cast<uv_handle_t*>(&async_));
        running_ = false;
    }

    void stop() noexcept {
        UVCO_CONTRACT_CHECK(isLoopThread());
        uv_stop(&loop_);
    }

  private:
    void unlink(detail::LoopCallback* const callback) noexcept {
        if (callback->previous != nullptr)
            callback->previous->next = callback->next;
        else
            callbacks_ = callback->next;
        if (callback->next != nullptr) callback->next->previous = callback->previous;
        callback->previous = nullptr;
        callback->next = nullptr;
        callback->queued = false;
    }

    static void onIdle(uv_idle_t* const idle) noexcept {
        static_cast<Loop*>(idle->data)->drainLocal();
    }

    static void onAsync(uv_async_t* const async) noexcept {
        static_cast<Loop*>(async->data)->drainRemote();
    }

    void drainLocal() noexcept {
        batch_.clear();
        batch_.swap(local_);
        if (local_.empty()) uv_idle_stop(&idle_);
        // resume 期间新排队的协程进入（已清空的）local_，由下一次 idle 回调处理。
        for (auto const coroutine : batch_)
            coroutine.resume();
        batch_.clear();
        if (not local_.empty()) uv_idle_start(&idle_, &Loop::onIdle);
    }

    void drainRemote() noexcept {
        // 取消节点一次只取一个：执行一个节点可能恢复协程并销毁其他节点（它们会在
        // 析构前 unpost 自己），因此不能把整条链表一次换出。
        for (;;) {
            detail::LoopCallback* callback = nullptr;
            {
                std::lock_guard<std::mutex> lock{mutex_};
                callback = callbacks_;
                if (callback == nullptr) break;
                unlink(callback);
            }
            callback->invoke(callback);
        }

        {
            std::lock_guard<std::mutex> lock{mutex_};
            remoteBatch_.clear();
            remoteBatch_.swap(remote_);
        }
        for (auto const coroutine : remoteBatch_)
            coroutine.resume();
        remoteBatch_.clear();
    }

    uv_loop_t loop_{};
    uv_async_t async_{};
    uv_idle_t idle_{};
    std::thread::id owner_;
    bool running_ = false;

    // loop 线程独占
    std::vector<co2::coroutine_handle<>> local_;
    std::vector<co2::coroutine_handle<>> batch_;
    std::vector<co2::coroutine_handle<>> remoteBatch_;

    // 跨线程，mutex_ 保护
    std::mutex mutex_;
    std::vector<co2::coroutine_handle<>> remote_;
    detail::LoopCallback* callbacks_ = nullptr;
};

} // namespace uvco
