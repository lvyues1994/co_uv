#pragma once

#include <string>
#include <system_error>
#include <type_traits>

#include <uv.h>

// libuv 错误码：负数 int（UV_E*）。uvco 用 Errc 给它一个强类型名字，并通过
// uv_category() 接入 std::error_code。
//
// 唯一的例外是 uvco 自己的 Errc::aborted（UVCO_EABORTED）：操作因句柄被 close()/stop()
// 或 Loop 析构而结束——与 stop_token 取消（Errc::canceled）区分开。它不是 libuv 码，
// strerror()/errorName()/uv_category() 单独处理它（libuv 对未知码会分配一个永不释放的字符串）。

// 远离 libuv 的取值范围（errno 取负，以及 -3000..-4095 的 UV__E* 映射码）。
#define UVCO_EABORTED (-30001)

namespace uvco {

// 值就是 libuv 的负数错误码；0 不是错误。这里只列出常用名字，任何 UV_E* 都可以
// 用 static_cast<Errc>(code) 表示。
enum class Errc : int {
    canceled = UV_ECANCELED,
    eof = UV_EOF,
    timed_out = UV_ETIMEDOUT,
    again = UV_EAGAIN,
    invalid_argument = UV_EINVAL,
    no_such_file = UV_ENOENT,
    busy = UV_EBUSY,
    connection_refused = UV_ECONNREFUSED,
    connection_reset = UV_ECONNRESET,
    broken_pipe = UV_EPIPE,
    address_in_use = UV_EADDRINUSE,
    not_supported = UV_ENOTSUP,
    no_memory = UV_ENOMEM,
    bad_descriptor = UV_EBADF, // 在已关闭（或因取消 connect 而内部关闭）的流上操作
    not_connected = UV_ENOTCONN,
    // uvco 专有：操作被句柄的 close()/stop() 或 Loop 析构中止（不是 stop_token 取消）。
    aborted = UVCO_EABORTED,
};

inline int toInt(Errc const errc) noexcept { return static_cast<int>(errc); }

// uv_strerror：libuv 自己的错误描述（静态存储，永不释放）。
inline char const* strerror(int const code) noexcept {
    if (code == UVCO_EABORTED) return "operation aborted (handle closed or loop destroyed)";
    return uv_strerror(code);
}
inline char const* strerror(Errc const errc) noexcept { return strerror(toInt(errc)); }

// uv_err_name：例如 "ECANCELED"。
inline char const* errorName(Errc const errc) noexcept {
    if (errc == Errc::aborted) return "UVCO_EABORTED";
    return uv_err_name(toInt(errc));
}

namespace detail {

struct UvCategory final : std::error_category {
    char const* name() const noexcept override { return "uv"; }
    std::string message(int const code) const override { return uvco::strerror(code); }
};

} // namespace detail

inline std::error_category const& uv_category() noexcept {
    static detail::UvCategory const category;
    return category;
}

inline std::error_code make_error_code(Errc const errc) noexcept {
    return std::error_code{toInt(errc), uv_category()};
}

} // namespace uvco

namespace std {
template <> struct is_error_code_enum<uvco::Errc> : true_type {};
} // namespace std
