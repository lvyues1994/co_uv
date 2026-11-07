#pragma once

#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"

// Result<T>：值或 libuv 错误码（C++14 没有 std::expected，这是一个轻量版本）。
//
//   - 从 T 或 Errc 隐式构造；Errc 必须是负数（0 不是错误）。
//   - explicit operator bool / hasValue()：是否持有值。
//   - value() / operator* / operator->：前置条件是持有值，否则为契约违规
//     （与 co2 一致：误用不是可恢复错误，不抛异常）。
//   - error()：前置条件是持有错误。errorCode() 在成功时返回空 error_code。
//   - message()：uv_strerror(error())，成功时为 ""。
//
// Result<void> 只携带“成功或 Errc”。

namespace uvco {

template <class T> class UVCO_NODISCARD Result;

namespace detail {

inline Errc checkedErrc(Errc const errc) noexcept {
    UVCO_CONTRACT_CHECK(toInt(errc) < 0);
    return errc;
}

} // namespace detail

template <class T> class UVCO_NODISCARD Result {
    static_assert(not std::is_reference<T>::value, "Result<T&> is not supported");
    static_assert(not std::is_same<typename std::decay<T>::type, Errc>::value,
                  "Result<Errc> is ambiguous");

  public:
    using value_type = T;

    template <class U = T, class = typename std::enable_if<
                               std::is_default_constructible<U>::value>::type>
    Result() noexcept(std::is_nothrow_default_constructible<T>::value)
        : hasValue_{true} {
        ::new (static_cast<void*>(&value_)) T();
    }

    Result(T const& value) : hasValue_{true} { ::new (static_cast<void*>(&value_)) T(value); }

    Result(T&& value) noexcept(std::is_nothrow_move_constructible<T>::value)
        : hasValue_{true} {
        ::new (static_cast<void*>(&value_)) T(std::move(value));
    }

    Result(Errc const errc) noexcept : errc_{detail::checkedErrc(errc)}, hasValue_{false} {}

    Result(Result const& other) : hasValue_{other.hasValue_} {
        if (hasValue_)
            ::new (static_cast<void*>(&value_)) T(other.value_);
        else
            errc_ = other.errc_;
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
        : hasValue_{other.hasValue_} {
        if (hasValue_)
            ::new (static_cast<void*>(&value_)) T(std::move(other.value_));
        else
            errc_ = other.errc_;
    }

    Result& operator=(Result const& other) {
        if (this != &other) assign(other);
        return *this;
    }

    Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value &&
        std::is_nothrow_move_assignable<T>::value) {
        if (this != &other) assign(std::move(other));
        return *this;
    }

    ~Result() { destroy(); }

    bool hasValue() const noexcept { return hasValue_; }
    explicit operator bool() const noexcept { return hasValue_; }

    T& value() & noexcept {
        UVCO_CONTRACT_CHECK(hasValue_);
        return value_;
    }
    T const& value() const& noexcept {
        UVCO_CONTRACT_CHECK(hasValue_);
        return value_;
    }
    T&& value() && noexcept {
        UVCO_CONTRACT_CHECK(hasValue_);
        return std::move(value_);
    }

    T& operator*() & noexcept { return value(); }
    T const& operator*() const& noexcept { return value(); }
    T&& operator*() && noexcept { return std::move(*this).value(); }
    T* operator->() noexcept { return &value(); }
    T const* operator->() const noexcept { return &value(); }

    template <class U> T valueOr(U&& fallback) const& {
        return hasValue_ ? value_ : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U> T valueOr(U&& fallback) && {
        return hasValue_ ? std::move(value_) : static_cast<T>(std::forward<U>(fallback));
    }

    Errc error() const noexcept {
        UVCO_CONTRACT_CHECK(not hasValue_);
        return errc_;
    }

    std::error_code errorCode() const noexcept {
        return hasValue_ ? std::error_code{} : make_error_code(errc_);
    }

    char const* message() const noexcept { return hasValue_ ? "" : strerror(errc_); }

  private:
    void destroy() noexcept {
        if (hasValue_) value_.~T();
    }

    template <class Other> void assign(Other&& other) {
        if (hasValue_ && other.hasValue_) {
            value_ = std::forward<Other>(other).value_;
            return;
        }
        destroy();
        hasValue_ = false;
        if (other.hasValue_) {
            try {
                ::new (static_cast<void*>(&value_)) T(std::forward<Other>(other).value_);
                hasValue_ = true;
            } catch (...) {
                // A throwing constructor may already have overwritten union bytes.
                errc_ = Errc::no_memory;
                throw;
            }
        } else {
            errc_ = other.errc_;
        }
    }

    union {
        T value_;
        Errc errc_;
    };
    bool hasValue_;
};

template <> class UVCO_NODISCARD Result<void> {
  public:
    using value_type = void;

    Result() noexcept = default;
    Result(Errc const errc) noexcept : errc_{toInt(detail::checkedErrc(errc))} {}

    bool hasValue() const noexcept { return errc_ == 0; }
    explicit operator bool() const noexcept { return errc_ == 0; }

    void value() const noexcept { UVCO_CONTRACT_CHECK(errc_ == 0); }

    Errc error() const noexcept {
        UVCO_CONTRACT_CHECK(errc_ != 0);
        return static_cast<Errc>(errc_);
    }

    std::error_code errorCode() const noexcept {
        return errc_ == 0 ? std::error_code{} : make_error_code(static_cast<Errc>(errc_));
    }

    char const* message() const noexcept {
        return errc_ == 0 ? "" : strerror(static_cast<Errc>(errc_));
    }

  private:
    int errc_{0};
};

// libuv 风格的状态码（0 或负数）→ Result<void>。
inline Result<void> fromStatus(int const status) noexcept {
    if (status == 0) return {};
    return Result<void>{static_cast<Errc>(status)};
}

// libuv 风格的“非负为值、负数为错误”（例如 uv_fs_t::result）→ Result<T>。
template <class T> Result<T> fromValueOrStatus(long long const status) {
    if (status < 0) return Result<T>{static_cast<Errc>(static_cast<int>(status))};
    return Result<T>{static_cast<T>(status)};
}

} // namespace uvco
