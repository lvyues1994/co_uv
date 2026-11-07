#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>
#include "uvco/core/result.hpp"
namespace uvco { namespace detail { namespace watcher {
struct DeliveryStats {
    std::size_t buffered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t overwritten = 0;
    std::uint64_t coalesced = 0;
    bool saturated = false;
};
// Pure delivery policies: no loop, cancellation, handles or coroutine resumption.
template<class T> struct DeliveryRecord {
    Result<T> result = Errc::aborted;
    std::exception_ptr exception;
};

template<class T> struct NoMerge {
    bool operator()(T&, T const&) const noexcept { return false; }
};

template<class T, std::size_t Capacity = 32, class Merge = NoMerge<T>, bool MergeErrors = true> class BoundedFifo {
    static_assert(Capacity != 0, "a delivery FIFO must be bounded and nonempty");
  public:
    using value_type = T;
    static constexpr std::size_t capacity() noexcept { return Capacity; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    void offer(DeliveryRecord<T> record) noexcept { push(std::move(record)); }
    DeliveryRecord<T> take() noexcept { return pop(); }
    DeliveryStats stats() const noexcept {
        auto const max = (std::numeric_limits<std::uint64_t>::max)();
        return DeliveryStats{size_, overflow_, 0, coalesced_, overflow_ == max || coalesced_ == max};
    }
    DeliveryRecord<T> const& front() const noexcept {
        UVCO_CONTRACT_CHECK(size_ != 0); return entries_[head_];
    }
    std::uint64_t overflowCount() const noexcept { return overflow_; }
    std::uint64_t coalescedCount() const noexcept { return coalesced_; }

    void push(DeliveryRecord<T> record) noexcept {
        if (size_ != 0) {
            auto& tail = entries_[(head_ + size_ - 1) % capacity()];
            if (!tail.exception && !record.exception &&
                ((tail.result && record.result && Merge{}(*tail.result, *record.result)) ||
                 (MergeErrors && !tail.result && !record.result && tail.result.error() == record.result.error()))) {
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

    DeliveryRecord<T> pop() noexcept {
        UVCO_CONTRACT_CHECK(size_ != 0);
        auto record = std::move(entries_[head_]);
        entries_[head_] = DeliveryRecord<T>{}; // release strings/exception ownership now
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
    std::array<DeliveryRecord<T>, capacity()> entries_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t overflow_ = 0;
    std::uint64_t coalesced_ = 0;
};


template<class T> class LatestOnly : public BoundedFifo<T, 1, NoMerge<T>, false> {
    using Base = BoundedFifo<T, 1, NoMerge<T>, false>;
  public:
    DeliveryStats stats() const noexcept {
        auto out = Base::stats(); out.overwritten = out.dropped; out.dropped = 0; return out;
    }
};
template<class T, class Merge> using ReduceOne = BoundedFifo<T, 1, Merge, false>;
struct WrapAdd {
    bool operator()(std::uint64_t& a, std::uint64_t b) const noexcept { a += b; return true; }
};
// A zero wrapped count is empty, matching Timer's historical uint64 semantics.
class LegacyTimerCount : public ReduceOne<std::uint64_t, WrapAdd> {
    using Base = ReduceOne<std::uint64_t, WrapAdd>;
  public:
    bool empty() const noexcept { return size() == 0; }
    DeliveryStats stats() const noexcept { auto out = Base::stats(); out.buffered = size(); return out; }
    std::size_t size() const noexcept {
        return Base::size() != 0 && (!front().result || front().exception || *front().result != 0) ? 1U : 0U;
    }
};
} } } // namespace uvco::detail::watcher
