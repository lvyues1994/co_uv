#pragma once
#include <type_traits>
#include <utility>
#include "uvco/core/co2_compat.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/result.hpp"
namespace uvco {
namespace detail {
// Loop-thread-only single waiter. Reserved from begin through take, including
// deferred completion. Deliberately owns neither HandleRef nor CancelHook:
// the module decides accounting, underlying stop and inline/deferred resumption.
// Callback payloads must be nothrow movable; allocating projections belong in
// request operations with explicit exception containment, not in this slot.
template <class T> class WaitSlot {
    static_assert(std::is_nothrow_move_constructible<Result<T>>::value &&
                      std::is_nothrow_move_assignable<Result<T>>::value,
                  "WaitSlot callback payload must be nothrow movable");

  public:
    struct Taken {
        Result<T> result;
        bool suspended;
    };
    WaitSlot() = default;
    WaitSlot(WaitSlot const &) = delete;
    WaitSlot &operator=(WaitSlot const &) = delete;
    bool idle() const noexcept {
        return state_ == State::idle;
    }
    bool pending() const noexcept {
        return state_ == State::pending;
    }
    bool done() const noexcept {
        return state_ == State::done;
    }
    void begin() noexcept {
        UVCO_CONTRACT_CHECK(idle());
        state_ = State::done;
        suspended_ = false;
        result_ = Result<T>{Errc::canceled};
    }
    void set(Result<T> result) noexcept {
        UVCO_CONTRACT_CHECK(done());
        result_ = std::move(result);
    }
    void pend(co2::coroutine_handle<> waiter) noexcept {
        UVCO_CONTRACT_CHECK(done());
        UVCO_CONTRACT_CHECK(static_cast<bool>(waiter));
        waiter_ = waiter;
        state_ = State::pending;
        suspended_ = true;
    }
    co2::coroutine_handle<> complete(Result<T> result) noexcept {
        UVCO_CONTRACT_CHECK(pending());
        result_ = std::move(result);
        state_ = State::done;
        return std::exchange(waiter_, {});
    }
    Taken take() noexcept {
        UVCO_CONTRACT_CHECK(done());
        Taken out{std::move(result_), suspended_};
        state_ = State::idle;
        suspended_ = false;
        waiter_ = nullptr;
        return out;
    }

  private:
    enum class State : unsigned char { idle, pending, done };
    State state_ = State::idle;
    Result<T> result_{Errc::canceled};
    co2::coroutine_handle<> waiter_;
    bool suspended_ = false;
};
}
} // namespace uvco::detail
