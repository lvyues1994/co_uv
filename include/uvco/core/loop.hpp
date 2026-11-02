#pragma once

#include <mutex>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif
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
// 保活（§4.1，借鉴 kotatsu 的 relay）：uv_async 默认 unref——它本身不让 uv_run 继续。
// 只有“跨线程工作还在路上”时才 ref：计数 keepAlive_ 由 run()（根 Task 未完成期间，
// 它可能跳到别的线程）和用户持有的 Loop::KeepAlive 构成。计数归零只在 async 回调末尾、
// 锁内复查“计数为零且远程队列为空”后才 unref；KeepAlive 在锁内递减并在锁内发送唤醒，
// 因此 loop 不可能在最后一次唤醒发出之前判定自己空闲、返回并被销毁。
//
// 所有权：用户持有 Loop（不可复制、不可移动）；run(loop, task) 只负责驱动。
// 析构前置条件：没有 run() 正在进行、没有 KeepAlive 存活。仍打开的句柄由析构兜底关闭
// （见 ~Loop）。

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

// 登记在 Loop 上的句柄控制块（HandleBlock）。~Loop 时：先 loopClosing()（发起关闭并
// 中止挂起的操作），等所有关闭完成后对仍存活者 loopGone()（之后它们不再触碰 Loop）。
class LoopResident {
  public:
    LoopResident(LoopResident const&) = delete;
    LoopResident& operator=(LoopResident const&) = delete;

  protected:
    LoopResident() noexcept = default;
    virtual ~LoopResident() = default;

    virtual void loopClosing() noexcept = 0;
    virtual void loopGone() noexcept = 0;

    // loop 线程：登记 / 注销（定义在 Loop 之后）。
    void linkTo(Loop& loop) noexcept;
    void unlinkFrom(Loop& loop) noexcept;

  private:
    friend class uvco::Loop;
    LoopResident* residentPrev_ = nullptr;
    LoopResident* residentNext_ = nullptr;
};

#ifndef _WIN32
// 每进程一次：SIGPIPE 仍为默认动作时忽略它——向已关闭的连接写入应返回 EPIPE，
// 而不是杀死进程。用户已设置的动作保持不变。
inline void ignoreSigpipeOnce() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action {};
        if (sigaction(SIGPIPE, nullptr, &action) != 0) return;
        if (action.sa_handler != SIG_DFL) return;
        action.sa_handler = SIG_IGN;
        static_cast<void>(sigaction(SIGPIPE, &action, nullptr));
    });
}
#endif

inline Loop*& currentLoopSlot() noexcept {
    thread_local Loop* loop = nullptr;
    return loop;
}

} // namespace detail

class Loop final : public co2::Scheduler {
  public:
    Loop() : owner_{std::this_thread::get_id()} {
        UVCO_CONTRACT_CHECK(detail::currentLoopSlot() == nullptr); // 每线程至多一个 Loop
#ifndef _WIN32
        detail::ignoreSigpipeOnce();
#endif
        int status = uv_loop_init(&loop_);
        UVCO_CONTRACT_CHECK(status == 0);
        loop_.data = this;

        status = uv_async_init(&loop_, &async_, &Loop::onAsync);
        UVCO_CONTRACT_CHECK(status == 0);
        async_.data = this;
        // async 默认不保活；keepAlive_ 计数非零时才 ref（见文件头）。
        uv_unref(reinterpret_cast<uv_handle_t*>(&async_));

        status = uv_idle_init(&loop_, &idle_);
        UVCO_CONTRACT_CHECK(status == 0);
        idle_.data = this;

        detail::currentLoopSlot() = this;
    }

    Loop(Loop const&) = delete;
    Loop& operator=(Loop const&) = delete;

    // 析构（借鉴 kotatsu 的 ~event_loop）：
    //   1. 还打开的 uvco 句柄 → closeInternally()，挂起的操作以 Errc::aborted 结束；
    //      其他（经 raw() 直接创建的）句柄 → uv_close，close 回调把 data 置空；
    //   2. UV_RUN_ONCE 直到 loop 不再存活：close 回调执行、被中止或被取消的请求回调、
    //      因此恢复的协程都在这里运行（它们新开的句柄也会被关闭）；
    //   3. 关闭内部的 async / idle，uv_loop_close 必须成功；
    //   4. 所有者仍存活的控制块变为“孤儿”：之后只能 close()/析构（直接释放，不触碰 Loop），
    //      其他操作是契约违规。
    // 带着存活的 KeepAlive 析构是契约违规（另一个线程还会投递）。
    ~Loop() override {
        UVCO_CONTRACT_CHECK(isLoopThread());
        UVCO_CONTRACT_CHECK(not running_);
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (keepAlive_ != 0) UVCO_CONTRACT_FAIL("uvco::Loop destroyed with a live KeepAlive");
        }

        for (;;) {
            closeResidents();
            uv_walk(&loop_, &Loop::closeForeign, this);
            if (uv_loop_alive(&loop_) == 0) break;
            static_cast<void>(uv_run(&loop_, UV_RUN_ONCE));
        }
        {
            std::lock_guard<std::mutex> lock{mutex_};
            // 远程工作只能来自没有持有 KeepAlive 的线程：它们与析构竞争，是契约违规。
            UVCO_CONTRACT_CHECK(remote_.empty() && callbacks_ == nullptr);
        }
        UVCO_CONTRACT_CHECK(local_.empty());

        uv_close(reinterpret_cast<uv_handle_t*>(&async_), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&idle_), nullptr);
        static_cast<void>(uv_run(&loop_, UV_RUN_NOWAIT)); // close 回调在同一次迭代末尾执行
        int const status = uv_loop_close(&loop_);
        if (status != 0) UVCO_CONTRACT_FAIL("uvco::Loop: uv_loop_close failed after closing every handle");

        while (residents_ != nullptr) {
            detail::LoopResident* const resident = residents_;
            resident->unlinkFrom(*this);
            resident->loopGone();
        }
        detail::currentLoopSlot() = nullptr;
    }

    // 保活令牌：存活期间 loop 不会因“无事可做”而让 uv_run 返回（见 drain()）。在 loop
    // 线程取得，可以移动到任意线程并在那里销毁；销毁时在锁内唤醒 loop 复查。
    // 用于把工作交给 loop 看不见的线程（线程池、外部回调）、且之后会 schedule()/post() 回来。
    class KeepAlive {
      public:
        KeepAlive() noexcept = default;
        KeepAlive(KeepAlive&& other) noexcept : loop_{std::exchange(other.loop_, nullptr)} {}
        KeepAlive& operator=(KeepAlive&& other) noexcept {
            if (this != &other) {
                Loop* const old = std::exchange(loop_, std::exchange(other.loop_, nullptr));
                if (old != nullptr) old->releaseKeepAlive();
            }
            return *this;
        }
        KeepAlive(KeepAlive const&) = delete;
        KeepAlive& operator=(KeepAlive const&) = delete;
        ~KeepAlive() {
            if (loop_ != nullptr) loop_->releaseKeepAlive();
        }

        explicit operator bool() const noexcept { return loop_ != nullptr; }
        void reset() noexcept { *this = KeepAlive{}; }

      private:
        friend class Loop;
        explicit KeepAlive(Loop* const loop) noexcept : loop_{loop} {}
        Loop* loop_ = nullptr;
    };

    // loop 线程：取得一个保活令牌。
    UVCO_NODISCARD KeepAlive keepAlive() {
        UVCO_CONTRACT_CHECK(isLoopThread());
        acquireKeepAlive();
        return KeepAlive{this};
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

    // 以 UV_RUN_DEFAULT 运行直到 stop() 被调用（在 loop 线程）。根 Task 未完成期间
    // run() 持有一份保活：协程可能正跑在 loop 看不见的线程上（scheduleOn(pool)），
    // uv_run 应阻塞等待它跳回来，而不是空转返回。
    void runUntilStopped(bool const& stopped) {
        UVCO_CONTRACT_CHECK(isLoopThread());
        UVCO_CONTRACT_CHECK(not running_); // uv_run 不可重入
        running_ = true;
        acquireKeepAlive();
        while (not stopped)
            uv_run(&loop_, UV_RUN_DEFAULT);
        {
            std::lock_guard<std::mutex> lock{mutex_};
            --keepAlive_;
            unrefIfIdleLocked();
        }
        running_ = false;
    }

    // 运行直到 loop 无事可做：没有活动的句柄 / 请求、没有排队的协程、没有存活的
    // KeepAlive。返回时不保证别的线程不再投递——那样的线程必须持有 KeepAlive。
    void runUntilIdle() {
        UVCO_CONTRACT_CHECK(isLoopThread());
        UVCO_CONTRACT_CHECK(not running_);
        running_ = true;
        while (uv_run(&loop_, UV_RUN_DEFAULT) != 0) {
        }
        running_ = false;
    }

    void stop() noexcept {
        UVCO_CONTRACT_CHECK(isLoopThread());
        uv_stop(&loop_);
    }

  private:
    friend class detail::LoopResident;

    void acquireKeepAlive() noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        if (keepAlive_++ == 0) uv_ref(reinterpret_cast<uv_handle_t*>(&async_));
    }

    // 任意线程。递减与唤醒都在锁内：loop 只在锁内复查后才 unref，于是它不可能在这次
    // 唤醒发出之前判定空闲、从 uv_run 返回并销毁 async_（kotatsu relay::release 同理）。
    void releaseKeepAlive() noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        UVCO_CONTRACT_CHECK(keepAlive_ != 0);
        --keepAlive_;
        uv_async_send(&async_);
    }

    // 持锁调用，loop 线程。
    void unrefIfIdleLocked() noexcept {
        if (keepAlive_ == 0 && remote_.empty() && callbacks_ == nullptr)
            uv_unref(reinterpret_cast<uv_handle_t*>(&async_));
    }

    void closeResidents() noexcept {
        for (detail::LoopResident* resident = residents_; resident != nullptr;) {
            detail::LoopResident* const next = resident->residentNext_;
            resident->loopClosing(); // 只发起关闭；释放发生在之后的 close 回调里
            resident = next;
        }
    }

    static void closeForeign(uv_handle_t* const handle, void* const arg) noexcept {
        Loop* const self = static_cast<Loop*>(arg);
        if (handle == reinterpret_cast<uv_handle_t*>(&self->async_) ||
            handle == reinterpret_cast<uv_handle_t*>(&self->idle_) || uv_is_closing(handle) != 0)
            return;
        uv_close(handle, [](uv_handle_t* const closed) { closed->data = nullptr; });
    }

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

        // 复查（kotatsu on_relay 同理）：排空期间别的线程可能又投递了工作、或刚放下最后
        // 一个 KeepAlive。只有计数为零且队列为空时才 unref；否则保持 ref，唤醒已在路上。
        std::lock_guard<std::mutex> lock{mutex_};
        if (not remote_.empty() || callbacks_ != nullptr)
            uv_ref(reinterpret_cast<uv_handle_t*>(&async_)); // 刚到的工作：下一次迭代处理
        else
            unrefIfIdleLocked();
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
    detail::LoopResident* residents_ = nullptr;

    // 跨线程，mutex_ 保护
    std::mutex mutex_;
    std::vector<co2::coroutine_handle<>> remote_;
    detail::LoopCallback* callbacks_ = nullptr;
    unsigned long keepAlive_ = 0;
};

namespace detail {

inline void LoopResident::linkTo(Loop& loop) noexcept {
    residentPrev_ = nullptr;
    residentNext_ = loop.residents_;
    if (loop.residents_ != nullptr) loop.residents_->residentPrev_ = this;
    loop.residents_ = this;
}

inline void LoopResident::unlinkFrom(Loop& loop) noexcept {
    if (residentPrev_ != nullptr)
        residentPrev_->residentNext_ = residentNext_;
    else
        loop.residents_ = residentNext_;
    if (residentNext_ != nullptr) residentNext_->residentPrev_ = residentPrev_;
    residentPrev_ = nullptr;
    residentNext_ = nullptr;
}

} // namespace detail
} // namespace uvco
