#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "uvco/detail/libuv.hpp"

#include "uvco/core/error.hpp"
#include "uvco/core/result.hpp"

// SocketAddress：IPv4/IPv6 地址 + 端口（值类型）。只解析数字 IP，不做 DNS（§4.6）。
//
//   auto addr = uvco::SocketAddress::parse("127.0.0.1", 8080);   // Result<SocketAddress>
//   addr->port(); addr->ip(); addr->toString();   // "127.0.0.1:8080" / "[::1]:8080"

namespace uvco {

class SocketAddress {
  public:
    // 未指定（AF_UNSPEC）。
    SocketAddress() noexcept { std::memset(&storage_, 0, sizeof storage_); }

    static Result<SocketAddress> ipv4(char const* const ip, std::uint16_t const port) noexcept {
        SocketAddress a;
        int const s = uv_ip4_addr(ip, port, reinterpret_cast<sockaddr_in*>(&a.storage_));
        if (s < 0) return static_cast<Errc>(s);
        return a;
    }

    static Result<SocketAddress> ipv6(char const* const ip, std::uint16_t const port) noexcept {
        SocketAddress a;
        int const s = uv_ip6_addr(ip, port, reinterpret_cast<sockaddr_in6*>(&a.storage_));
        if (s < 0) return static_cast<Errc>(s);
        return a;
    }

    // 含 ':' 按 IPv6，否则按 IPv4。
    static Result<SocketAddress> parse(char const* const ip, std::uint16_t const port) noexcept {
        return std::strchr(ip, ':') != nullptr ? ipv6(ip, port) : ipv4(ip, port);
    }
    static Result<SocketAddress> parse(std::string const& ip, std::uint16_t const port) noexcept {
        return parse(ip.c_str(), port);
    }

    // 从 getsockname / getpeername 的结果构造；未知族得到 AF_UNSPEC。
    static SocketAddress fromRaw(sockaddr const* const raw) noexcept {
        SocketAddress a;
        if (raw->sa_family == AF_INET)
            std::memcpy(&a.storage_, raw, sizeof(sockaddr_in));
        else if (raw->sa_family == AF_INET6)
            std::memcpy(&a.storage_, raw, sizeof(sockaddr_in6));
        return a;
    }

    int family() const noexcept { return storage_.ss_family; }
    bool isIpv4() const noexcept { return family() == AF_INET; }
    bool isIpv6() const noexcept { return family() == AF_INET6; }

    sockaddr const* raw() const noexcept { return reinterpret_cast<sockaddr const*>(&storage_); }

    std::uint16_t port() const noexcept {
        unsigned char const* p = nullptr;
        if (isIpv4())
            p = reinterpret_cast<unsigned char const*>(
                &reinterpret_cast<sockaddr_in const*>(&storage_)->sin_port);
        else if (isIpv6())
            p = reinterpret_cast<unsigned char const*>(
                &reinterpret_cast<sockaddr_in6 const*>(&storage_)->sin6_port);
        else
            return 0;
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]); // 网络字节序
    }

    std::string ip() const {
        char text[64] = {};
        int s = UV_EINVAL;
        if (isIpv4())
            s = uv_ip4_name(reinterpret_cast<sockaddr_in const*>(&storage_), text, sizeof text);
        else if (isIpv6())
            s = uv_ip6_name(reinterpret_cast<sockaddr_in6 const*>(&storage_), text, sizeof text);
        return s == 0 ? std::string{text} : std::string{};
    }

    std::string toString() const {
        if (isIpv6()) return "[" + ip() + "]:" + std::to_string(port());
        if (isIpv4()) return ip() + ":" + std::to_string(port());
        return "unspecified";
    }

  private:
    sockaddr_storage storage_;
};

} // namespace uvco
