#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>
#include "uvco/core/cancel.hpp"
#include "uvco/core/co2_compat.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/result.hpp"
#include "uvco/core/wait_slot.hpp"

namespace uvco {
namespace fs {
namespace detail {

// A watch is a lossy notification channel, not a filesystem journal. Fixed
// storage bounds allocations by record count; filenames remain owned strings.
template<class T> struct WatchRecord {
    Result<T> result = Errc::aborted;
    std::exception_ptr exception;
};

template<class T> struct NoWatchMerge {
    bool operator()(T&, T const&) const noexcept { return false; }
};

template<class T, class Merge = NoWatchMerge<T>> class WatchQueue {
  public:
    static constexpr std::size_t capacity() noexcept { return 32; }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t overflowCount() const noexcept { return overflow_; }
    std::uint64_t coalescedCount() const noexcept { return coalesced_; }

    void push(WatchRecord<T> record) noexcept {
        if (size_ != 0) {
            auto& tail = entries_[(head_ + size_ - 1) % capacity()];
            if (!tail.exception && !record.exception &&
                ((tail.result && record.result && Merge{}(*tail.result, *record.result)) ||
                 (!tail.result && !record.result && tail.result.error() == record.result.error()))) {
                increment(coalesced_);
                return;
            }
        }
        if (size_ == capacity()) {
            entries_[head_] = std::move(record);
            head_ = (head_ + 1) % capacity();
            increment(overflow_);
        } else {
            entries_[(head_ + size_) % capacity()] = std::move(record);
            ++size_;
        }
    }

    WatchRecord<T> pop() noexcept {
        UVCO_CONTRACT_CHECK(size_ != 0);
        auto record = std::move(entries_[head_]);
        entries_[head_] = WatchRecord<T>{}; // release strings/exception ownership now
        head_ = (head_ + 1) % capacity();
        --size_;
        return record;
    }

    void clear() noexcept {
        while (size_ != 0) static_cast<void>(pop());
        head_ = 0;
    }

  private:
    static void increment(std::uint64_t& counter) noexcept {
        if (counter != (std::numeric_limits<std::uint64_t>::max)()) ++counter;
    }
    std::array<WatchRecord<T>, capacity()> entries_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t overflow_ = 0;
    std::uint64_t coalesced_ = 0;
};

// HandleRef is held by the awaiter from construction. WaitSlot reserves the
// single waiter until await_resume, including deferred completion after close.
template<class T, class Merge = NoWatchMerge<T>>
class WatchBlock : public ::uvco::detail::HandleBlock {
  public:
    using Info = T;
    static constexpr std::size_t bufferCapacity() noexcept { return WatchQueue<T, Merge>::capacity(); }
    std::size_t queued() const noexcept { return queue_.size(); }
    std::uint64_t overflowCount() const noexcept { return queue_.overflowCount(); }
    std::uint64_t coalescedCount() const noexcept { return queue_.coalescedCount(); }
    bool isActive() noexcept { return !closing() && uv_is_active(uvHandle()) != 0; }

    bool suspendNext(co2::coroutine_handle<> waiter, co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        UVCO_CONTRACT_CHECK(slot_.idle());
        slot_.begin();
        if (closing()) {
            slot_.set(Errc::aborted);
            return false;
        }
        // A pre-canceled wait does not consume queued notifications.
        if (token.stop_requested()) {
            slot_.set(Errc::canceled);
            return false;
        }
        if (queue_.size() != 0) {
            auto record = queue_.pop();
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
        cancel_.arm(loop(), std::move(token), &WatchBlock::onCancel, this);
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
    explicit WatchBlock(Loop& loop) noexcept : HandleBlock{loop} {}

    // Call only from the loop/native callback. Never access this after publish:
    // completion resumes inline and may close/restart/destroy the public owner.
    void publish(Result<T> result, std::exception_ptr exception = {}) noexcept {
        // A backend may deactivate before delivering its final error. Only
        // closing invalidates delivery; next() drains buffered final statuses.
        if (closing()) return;
        if (slot_.pending()) {
            exception_ = std::move(exception);
            auto waiter = slot_.complete(std::move(result));
            waiter.resume();
            return;
        }
        queue_.push(WatchRecord<T>{std::move(result), std::move(exception)});
    }

    void abortPending() noexcept override {
        queue_.clear();
        if (slot_.pending()) {
            auto waiter = slot_.complete(Errc::aborted);
            resumeLater(waiter);
        }
    }

  private:
    static void onCancel(void* context) noexcept {
        auto& self = *static_cast<WatchBlock*>(context);
        if (!self.slot_.pending()) return; // first loop completion wins
        auto waiter = self.slot_.complete(Errc::canceled);
        waiter.resume(); // no self access afterwards
    }
    WatchQueue<T, Merge> queue_;
    ::uvco::detail::WaitSlot<T> slot_;
    ::uvco::detail::CancelHook cancel_;
    std::exception_ptr exception_;
};

template<class Block> class WatchAwaiter {
  public:
    explicit WatchAwaiter(Block& block) noexcept : block_{&block} {}
    bool await_ready() const noexcept { return false; }
    template<class Promise> bool await_suspend(co2::coroutine_handle<Promise> waiter) {
        return block_->suspendNext(waiter, ::uvco::detail::co2compat::stopTokenOf(waiter));
    }
    Result<typename Block::Info> await_resume() { return block_->resumeNext(); }
  private:
    ::uvco::detail::HandleRef<Block> block_;
};

} // namespace detail
} // namespace fs
} // namespace uvco
