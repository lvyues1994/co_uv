#pragma once

#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/sync/wait_queue.hpp"

// Event：loop 内的手动复位事件（docs/architecture.md §4.5）。
//
//   uvco::Event ready{loop};
//   CO2_AWAIT_SET(r, ready.wait());   // Result<void>
//   ready.set();                      // 唤醒全部等待者（延后到 loop 恢复），之后的 wait() 立即完成
//   ready.reset();                    // 回到未触发
//
//   - 只能在 loop 线程上使用（契约）；跨线程请先 scheduleOn(loop)。
//   - wait()：已请求停止 → UV_ECANCELED（即使已 set）；已 set → 立即成功；否则挂起，
//     stop_token 被请求 → UV_ECANCELED。
//   - set() 之后立即 reset()：已被 set() 唤醒的等待者仍然成功（唤醒时就确定了结果）。
//   - 带着等待者销毁 Event 是契约违规。

namespace uvco {

class Event {
  public:
    explicit Event(Loop& loop, bool const initiallySet = false) noexcept
        : waiters_{loop}, set_{initiallySet} {}

    Event(Event const&) = delete;
    Event& operator=(Event const&) = delete;

    Loop& loop() const noexcept { return waiters_.loop(); }

    bool isSet() const noexcept { return set_; }

    void set() noexcept {
        waiters_.checkThread();
        set_ = true;
        waiters_.wakeAll(0);
    }

    void reset() noexcept {
        waiters_.checkThread();
        set_ = false;
    }

    class WaitAwaiter {
      public:
        explicit WaitAwaiter(Event& event) noexcept : event_{&event} {}

        bool await_ready() const noexcept { return false; }

        template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
            Event& event = *event_;
            event.waiters_.checkThread();
            co2::stop_token token = co2::detail::stopTokenOf(waiting);
            if (token.stop_requested()) {
                node_.status = UV_ECANCELED;
                return false;
            }
            if (event.set_) {
                node_.status = 0;
                return false;
            }
            event.waiters_.enqueue(node_, waiting, std::move(token));
            return true;
        }

        Result<void> await_resume() noexcept {
            node_.finish();
            return fromStatus(node_.status);
        }

      private:
        Event* event_;
        detail::WaitNode node_;
    };

    UVCO_NODISCARD WaitAwaiter wait() noexcept { return WaitAwaiter{*this}; }

  private:
    detail::WaitQueue waiters_;
    bool set_;
};

} // namespace uvco
