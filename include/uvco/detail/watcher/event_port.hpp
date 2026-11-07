#pragma once
#include "uvco/core/cancel.hpp"
#include "uvco/core/co2_compat.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/wait_slot.hpp"
#include "uvco/detail/watcher/delivery.hpp"
namespace uvco { namespace detail { namespace watcher {
// HandleRef is held by the awaiter from construction. WaitSlot reserves the
// single waiter until await_resume, including deferred completion after close.
struct InlineNative { static constexpr bool deferred = false; };
struct DeferredNative { static constexpr bool deferred = true; };
struct NormalRules {
    static constexpr bool closingFirst = true;
    static constexpr bool clearOnClose = true;
};
struct LegacyTimerRules {
    static constexpr bool closingFirst = false;
    static constexpr bool clearOnClose = false;
};
template<class T, class Buffer, class ResumePolicy = InlineNative, class Rules = NormalRules>
class EventPort : public ::uvco::detail::HandleBlock {
  public:
    using Info = T;
    static constexpr std::size_t bufferCapacity() noexcept { return Buffer::capacity(); }
    std::size_t queued() const noexcept { return queue_.size(); }
    DeliveryStats stats() const noexcept { return queue_.stats(); }
    std::uint64_t overflowCount() const noexcept { return queue_.overflowCount(); }
    std::uint64_t coalescedCount() const noexcept { return queue_.coalescedCount(); }
    bool isActive() noexcept { return !closing() && uv_is_active(uvHandle()) != 0; }

    bool suspendNext(co2::coroutine_handle<> waiter, co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        UVCO_CONTRACT_CHECK(slot_.idle());
        slot_.begin();
        if (Rules::closingFirst && closing()) {
            slot_.set(Errc::aborted);
            return false;
        }
        // A pre-canceled wait does not consume queued notifications.
        if (token.stop_requested()) {
            slot_.set(Errc::canceled);
            return false;
        }
        if (!queue_.empty()) {
            auto record = queue_.take();
            exception_ = std::move(record.exception);
            slot_.set(std::move(record.result));
            return false;
        }
        if (!isActive()) {
            slot_.set(Errc::invalid_argument);
            return false;
        }
        slot_.pend(waiter);
        opBegin();
        cancel_.arm(loop(), std::move(token), &EventPort::onCancel, this);
        return true;
    }

    Result<T> resumeNext() {
        cancel_.disarm();
        auto taken = slot_.take();
        auto exception = std::exchange(exception_, {});
        if (taken.suspended) opEnd(); // no this access after opEnd
        if (exception) std::rethrow_exception(exception);
        return std::move(taken.result);
    }

  protected:
    explicit EventPort(Loop& loop) noexcept : HandleBlock{loop} {}

    // Call only from the loop/native callback. Never access this after publish:
    // completion resumes inline and may close/restart/destroy the public owner.
    void publish(Result<T> result, std::exception_ptr exception = {}) noexcept {
        // A backend may deactivate before delivering its final error. Only
        // closing invalidates delivery; next() drains buffered final statuses.
        if (closing()) return;
        queue_.offer(DeliveryRecord<T>{std::move(result), std::move(exception)});
        if (slot_.pending() && !queue_.empty()) {
            auto record = queue_.take();
            exception_ = std::move(record.exception);
            auto waiter = slot_.complete(std::move(record.result));
            if (ResumePolicy::deferred) resumeLater(waiter);
            else waiter.resume(); // no this access afterwards
        }
    }

    void abortPending() noexcept override {
        if (Rules::clearOnClose) queue_.clear();
        abortWaiter();
    }
    void clearBuffer() noexcept { queue_.clear(); }
    void stopDelivery() noexcept { queue_.clear(); abortWaiter(); }

  private:
    void abortWaiter() noexcept {
        if (slot_.pending()) {
            auto waiter = slot_.complete(Errc::aborted);
            resumeLater(waiter);
        }
    }

  private:
    static void onCancel(void* context) noexcept {
        auto& self = *static_cast<EventPort*>(context);
        if (!self.slot_.pending()) return; // first loop completion wins
        auto waiter = self.slot_.complete(Errc::canceled);
        waiter.resume(); // no self access afterwards
    }
    Buffer queue_;
    ::uvco::detail::WaitSlot<T> slot_;
    ::uvco::detail::CancelHook cancel_;
    std::exception_ptr exception_;
};

template<class Block> class NextAwaiter {
  public:
    explicit NextAwaiter(Block& block) noexcept : block_{&block} {}
    bool await_ready() const noexcept { return false; }
    template<class Promise> bool await_suspend(co2::coroutine_handle<Promise> waiter) {
        return block_->suspendNext(waiter, ::uvco::detail::co2compat::stopTokenOf(waiter));
    }
    Result<typename Block::Info> await_resume() { return block_->resumeNext(); }
  private:
    ::uvco::detail::HandleRef<Block> block_;
};

} } } // namespace uvco::detail::watcher
