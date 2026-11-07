#pragma once

#include "uvco/core/co2_compat.hpp"

#include <type_traits>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/sync/wait_queue.hpp"

// Mutex：loop 内的协程互斥锁，严格 FIFO（docs/architecture.md §4.5）。
//
//   uvco::Mutex mutex{loop};
//   CO2_AWAIT_SET(guard, mutex.scopedLock());   // Result<Mutex::Guard>；guard 析构时解锁
//   CO2_AWAIT_SET(r, mutex.lock());             // Result<void>；之后手动 mutex.unlock()
//   if (mutex.tryLock()) { ...; mutex.unlock(); }
//
//   - 只能在 loop 线程上使用（契约）。不可重入：持有者再次 lock() 会永远等待。
//   - 公平：unlock() 时若有等待者，所有权**直接移交**给队首（锁保持占用），队首延后到
//     loop 恢复；期间新来的 lock()/tryLock() 不能插队。
//   - lock()：已请求停止 → UV_ECANCELED（即使锁空闲）；挂起中 stop_token 被请求 →
//     UV_ECANCELED 并离开队列。已获得移交的等待者不会再被取消。
//   - 未持有时 unlock()、带着等待者或在持有状态下销毁 Mutex 是契约违规。

namespace uvco {

class Mutex {
  public:
    explicit Mutex(Loop& loop) noexcept : waiters_{loop} {}

    Mutex(Mutex const&) = delete;
    Mutex& operator=(Mutex const&) = delete;

    ~Mutex() { UVCO_CONTRACT_CHECK(not locked_); }

    Loop& loop() const noexcept { return waiters_.loop(); }

    bool isLocked() const noexcept { return locked_; }

    bool tryLock() noexcept {
        waiters_.checkThread();
        if (locked_) return false;
        locked_ = true;
        return true;
    }

    void unlock() noexcept {
        waiters_.checkThread();
        UVCO_CONTRACT_CHECK(locked_);
        if (waiters_.empty()) {
            locked_ = false;
            return;
        }
        waiters_.wakeFront(0); // 移交：locked_ 保持为 true
    }

    // 作用域锁：只可移动；析构（或 unlock()）时解锁。
    class Guard {
      public:
        Guard() noexcept = default;
        explicit Guard(Mutex& mutex) noexcept : mutex_{&mutex} {}
        Guard(Guard&& other) noexcept : mutex_{std::exchange(other.mutex_, nullptr)} {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                unlock();
                mutex_ = std::exchange(other.mutex_, nullptr);
            }
            return *this;
        }
        Guard(Guard const&) = delete;
        Guard& operator=(Guard const&) = delete;
        ~Guard() { unlock(); }

        bool ownsLock() const noexcept { return mutex_ != nullptr; }
        explicit operator bool() const noexcept { return ownsLock(); }

        void unlock() noexcept {
            if (mutex_ != nullptr) std::exchange(mutex_, nullptr)->unlock();
        }

      private:
        Mutex* mutex_ = nullptr;
    };

    template <class T> class LockAwaiter {
      public:
        explicit LockAwaiter(Mutex& mutex) noexcept : mutex_{&mutex} {}

        bool await_ready() const noexcept { return false; }

        template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
            Mutex& mutex = *mutex_;
            mutex.waiters_.checkThread();
            co2::stop_token token = ::uvco::detail::co2compat::stopTokenOf(waiting);
            if (token.stop_requested()) {
                node_.status = UV_ECANCELED;
                return false;
            }
            if (mutex.tryLock()) {
                node_.status = 0;
                return false;
            }
            mutex.waiters_.enqueue(node_, waiting, std::move(token));
            return true;
        }

        Result<T> await_resume() noexcept {
            node_.finish();
            if (node_.status < 0) return Result<T>{static_cast<Errc>(node_.status)};
            return make(std::is_void<T>{});
        }

      private:
        Result<T> make(std::true_type) noexcept { return {}; }
        Result<T> make(std::false_type) noexcept { return Result<T>{Guard{*mutex_}}; }

        Mutex* mutex_;
        detail::WaitNode node_;
    };

    UVCO_NODISCARD LockAwaiter<void> lock() noexcept { return LockAwaiter<void>{*this}; }
    UVCO_NODISCARD LockAwaiter<Guard> scopedLock() noexcept { return LockAwaiter<Guard>{*this}; }

  private:
    detail::WaitQueue waiters_;
    bool locked_ = false;
};

} // namespace uvco
