#pragma once

#include <chrono>
#include <utility>

#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/address.hpp"
#include "uvco/stream/stream.hpp"

// Tcp：TCP 流（docs/architecture.md §4.6），只可移动。不做 DNS：地址必须是数字 IP。
//
//   uvco::Tcp server{loop};
//   server.bind(*uvco::SocketAddress::parse("127.0.0.1", 0));
//   server.listen();
//   CO2_AWAIT_SET(client, server.accept());          // Result<Tcp>
//
//   uvco::Tcp tcp{loop};
//   CO2_AWAIT_SET(r, tcp.connect(addr));              // Result<void>
//   CO2_AWAIT_SET(n, tcp.read(uvco::buffer(buf)));    // Result<std::size_t>，Errc::eof 表示结束
//   CO2_AWAIT_SET(r, tcp.write(uvco::buffer(data)));  // Result<void>
//
// connect 被取消时 libuv 无法撤回连接请求，于是整个 Tcp 被关闭：connect 返回
// ECANCELED（同一个流上其他挂起的操作以 Errc::aborted 结束），之后的操作返回
// Errc::bad_descriptor，只能 close()/析构。

namespace uvco {

class Tcp;

namespace detail {

class TcpBlock final : public StreamBlock {
  public:
    explicit TcpBlock(Loop& loop) : StreamBlock{loop} {
        int const status = uv_tcp_init(loop.raw(), &tcp_);
        UVCO_CONTRACT_CHECK(status == 0);
        bindHandle();
    }

    uv_tcp_t* tcp() noexcept { return &tcp_; }

  protected:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&tcp_); }
    StreamBlock* newPeer() override { return new TcpBlock{loop()}; }

  private:
    uv_tcp_t tcp_{};
};

class TcpConnectAwaiter final : public ConnectAwaiterBase<TcpConnectAwaiter> {
  public:
    TcpConnectAwaiter(TcpBlock& block, SocketAddress const& address) noexcept
        : ConnectAwaiterBase{block}, address_{address} {}

    int startOp(uv_connect_t* const req) noexcept {
        return uv_tcp_connect(req, static_cast<TcpBlock*>(static_cast<StreamBlock*>(block_))->tcp(), address_.raw(),
                              &TcpConnectAwaiter::onDone);
    }

  private:
    SocketAddress address_;
};

} // namespace detail

class Tcp : public detail::StreamHandle<detail::TcpBlock, Tcp> {
    using Base = detail::StreamHandle<detail::TcpBlock, Tcp>;

  public:
    // 空句柄（isOpen() == false），可作为帧局部变量的初值。
    Tcp() noexcept = default;
    explicit Tcp(Loop& loop) : Base{new detail::TcpBlock{loop}} {}

    // flags：例如 UV_TCP_IPV6ONLY。
    Result<void> bind(SocketAddress const& address, unsigned const flags = 0) noexcept {
        return fromStatus(uv_tcp_bind(raw(), address.raw(), flags));
    }

    UVCO_NODISCARD detail::TcpConnectAwaiter connect(SocketAddress const& address) noexcept {
        return detail::TcpConnectAwaiter{block(), address};
    }

    Result<void> setNoDelay(bool const enable) noexcept {
        return fromStatus(uv_tcp_nodelay(raw(), enable ? 1 : 0));
    }

    // delay：开启时，连接空闲多久后开始发送 keepalive 探测（向下取整到秒，至少 1 秒）。
    Result<void> setKeepAlive(bool const enable,
                              std::chrono::seconds const delay = std::chrono::seconds{60}) noexcept {
        unsigned const seconds =
            delay.count() < 1 ? 1u : static_cast<unsigned>(delay.count());
        return fromStatus(uv_tcp_keepalive(raw(), enable ? 1 : 0, seconds));
    }

    Result<SocketAddress> localAddress() const noexcept {
        sockaddr_storage storage;
        int length = static_cast<int>(sizeof storage);
        int const status =
            uv_tcp_getsockname(raw(), reinterpret_cast<sockaddr*>(&storage), &length);
        if (status < 0) return static_cast<Errc>(status);
        return SocketAddress::fromRaw(reinterpret_cast<sockaddr const*>(&storage));
    }

    Result<SocketAddress> peerAddress() const noexcept {
        sockaddr_storage storage;
        int length = static_cast<int>(sizeof storage);
        int const status =
            uv_tcp_getpeername(raw(), reinterpret_cast<sockaddr*>(&storage), &length);
        if (status < 0) return static_cast<Errc>(status);
        return SocketAddress::fromRaw(reinterpret_cast<sockaddr const*>(&storage));
    }

    // 底层 uv_tcp_t（loop 线程上使用；不要 uv_close 它）。
    uv_tcp_t* raw() const noexcept { return block().tcp(); }

  private:
    explicit Tcp(detail::TcpBlock* const block) noexcept : Base{block} {}
    template <class, class> friend class detail::AcceptAwaiter;
};

} // namespace uvco
