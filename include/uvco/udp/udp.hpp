#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#include <uv.h>

#include <co2/co2.hpp>

#include "uvco/core/cancel.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/address.hpp"
#include "uvco/stream/buffer.hpp"

// Udp：UDP 套接字（docs/architecture.md §4.7），只可移动。地址只接受数字 IP。
//
//   uvco::Udp udp{loop};
//   udp.bind(*uvco::SocketAddress::parse("127.0.0.1", 0));
//   CO2_AWAIT_SET(r, udp.send(peer, uvco::buffer(msg)));   // Result<void>
//   CO2_AWAIT_SET(d, udp.recv(uvco::buffer(buf)));         // Result<uvco::Datagram>
//   d->size; d->sender; d->truncated;
//
// 拉模式：只有 recv() 挂起时才接收，每次 recv() 取一个数据报，直接读进调用者的缓冲区。
// 没有等待者时到达的数据报不排队，留在内核的套接字缓冲区里（满了由内核丢弃）。
// 缓冲区比数据报小时多余部分被丢弃，truncated 为 true。
//
// recv 的完成总是推迟到下一轮（经 loop 调度）恢复，停止接收也推迟到 recv 回调之外：
// 不在 libuv 的 recv 回调里 uv_udp_recv_stop（Linux 上 libuv 在 EPOLLERR 之后读错误队列时
// 断言套接字仍在接收，参考 kotatsu src/async/io/udp.cpp）。
//
// 取消 recv → Errc::canceled；close() / Loop 析构 → 挂起的 recv / send 以 Errc::aborted 结束。
// send 在发起后不可取消。

namespace uvco {

struct Datagram {
    std::size_t size = 0;  // 写进缓冲区的字节数
    SocketAddress sender;  // 来源地址
    bool truncated = false; // 数据报比缓冲区大，多余部分已丢弃
};

class Udp;

namespace detail {

class UdpBlock final : public HandleBlock {
  public:
    explicit UdpBlock(Loop& loop) : HandleBlock{loop} {
        int const status = uv_udp_init(loop.raw(), &udp_);
        UVCO_CONTRACT_CHECK(status == 0);
        bindHandle();
        stopFrame_.owner = this;
    }

    uv_udp_t* udp() noexcept { return &udp_; }

    void beginOp() noexcept { opBegin(); }
    void endOp() noexcept { opEnd(); }

    bool suspendRecv(co2::coroutine_handle<> const waiter, MutableBuffer const buf,
                     co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        if (recvState_ != State::idle)
            UVCO_CONTRACT_FAIL("uvco udp: concurrent recv() on the same socket");
        UVCO_CONTRACT_CHECK(buf.data != nullptr && buf.size != 0);
        recvState_ = State::done;
        recvSuspended_ = false;
        recvTruncated_ = false;
        if (token.stop_requested()) {
            recvStatus_ = UV_ECANCELED;
            return false;
        }
        if (closing()) {
            recvStatus_ = UV_EBADF;
            return false;
        }
        if (recvError_ != 0) { // 没有等待者时收到的错误，交给下一个 recv
            recvStatus_ = std::exchange(recvError_, 0);
            return false;
        }
        if (not receiving_) {
            // 这里不可能在本套接字的 recv 回调里：recv 的完成总是推迟恢复。
            int const status = uv_udp_recv_start(&udp_, &UdpBlock::onAlloc, &UdpBlock::onRecv);
            if (status < 0) {
                recvStatus_ = status;
                return false;
            }
            receiving_ = true;
        }
        recvBuf_ = buf;
        recvWaiter_ = waiter;
        recvState_ = State::pending;
        recvSuspended_ = true;
        opBegin();
        recvCancel_.arm(loop(), std::move(token), &UdpBlock::onRecvCancel, this);
        return true;
    }

    Result<Datagram> resumeRecv() noexcept {
        UVCO_CONTRACT_CHECK(recvState_ == State::done);
        recvCancel_.disarm();
        ssize_t const status = recvStatus_;
        bool const suspended = recvSuspended_;
        Datagram d;
        if (status >= 0) {
            d.size = static_cast<std::size_t>(status);
            d.sender = recvSender_;
            d.truncated = recvTruncated_;
        }
        Result<Datagram> result = status < 0
                                      ? Result<Datagram>{static_cast<Errc>(static_cast<int>(status))}
                                      : Result<Datagram>{d};
        recvState_ = State::idle;
        recvSuspended_ = false;
        recvWaiter_ = nullptr;
        recvBuf_ = MutableBuffer{};
        if (suspended) opEnd(); // 最后一步：之后控制块可能已被释放
        return result;
    }

  protected:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&udp_); }

    void abortPending() noexcept override {
        receiving_ = false; // uv_close 已经停止接收
        if (recvState_ == State::pending) finishRecv(UVCO_EABORTED);
    }

  private:
    enum class State : unsigned char { idle, pending, done };

    // 推迟的“没有等待者就停止接收”：一个手写、不占堆的协程帧，经 loop.schedule 在下一轮执行
    // （与 run() 的完成帧同一手法）。排队期间以 holdBegin() 保证控制块活着（不妨碍句柄析构）。
    struct StopFrame {
        co2::detail::FrameHeader header{&co2::detail::noopDestroy, &UdpBlock::onStopStep, false,
                                        false};
        UdpBlock* owner = nullptr;
    };
    static_assert(std::is_standard_layout<StopFrame>::value,
                  "the stop frame must stay standard-layout");

    static constexpr std::size_t maxBufLen() noexcept {
        using Len = decltype(uv_buf_t{}.len);
        return static_cast<std::size_t>(std::numeric_limits<Len>::max()) <
                       std::numeric_limits<std::size_t>::max()
                   ? static_cast<std::size_t>(std::numeric_limits<Len>::max())
                   : std::numeric_limits<std::size_t>::max();
    }

    static void onAlloc(uv_handle_t* const handle, std::size_t, uv_buf_t* const buf) noexcept {
        UdpBlock& self = fromData<UdpBlock>(handle->data);
        if (self.recvState_ == State::pending) {
            std::size_t const size =
                self.recvBuf_.size < maxBufLen() ? self.recvBuf_.size : maxBufLen();
            buf->base = self.recvBuf_.data; // 直接读进等待者的缓冲区
            buf->len = static_cast<decltype(buf->len)>(size);
        } else {
            // 没有等待者（或本轮已经交付过一个数据报）：不取走数据，libuv 以 UV_ENOBUFS 回调。
            buf->base = nullptr;
            buf->len = 0;
        }
    }

    static void onRecv(uv_udp_t* const handle, ssize_t const nread, uv_buf_t const*,
                       sockaddr const* const addr, unsigned const flags) noexcept {
        UdpBlock& self = fromData<UdpBlock>(handle->data);
        // 0 字节且没有地址：套接字已读空，不是空数据报（空数据报总带着来源地址）。
        if (nread == 0 && addr == nullptr) return;
        // 本回调里绝不停止接收、绝不恢复协程：都推迟到下一轮。
        if (self.recvState_ != State::pending) {
            // 没有交出过缓冲区，不可能读到数据：UV_ENOBUFS（数据报留在内核里），或错误。
            if (nread < 0 && nread != UV_ENOBUFS) self.recvError_ = static_cast<int>(nread);
            self.requestStop();
            return;
        }
        if (nread >= 0) {
            self.recvSender_ = SocketAddress::fromRaw(addr);
            self.recvTruncated_ = (flags & UV_UDP_PARTIAL) != 0;
        }
        self.finishRecv(nread);
    }

    static void onRecvCancel(void* const context) noexcept {
        auto& self = *static_cast<UdpBlock*>(context);
        if (self.recvState_ != State::pending) return;
        // 在 loop 的投递回调里，不在 recv 回调里：可以直接停止。先停止，再恢复——
        // 被恢复的协程可能立刻再次 recv()。
        if (self.receiving_) {
            static_cast<void>(uv_udp_recv_stop(&self.udp_));
            self.receiving_ = false;
        }
        self.recvStatus_ = UV_ECANCELED;
        self.recvState_ = State::done;
        self.recvWaiter_.resume(); // 之后不再访问 self
    }

    // 总是推迟恢复：既用于 recv 回调，也用于 close()（用户调用栈）。
    void finishRecv(ssize_t const status) noexcept {
        recvStatus_ = status;
        recvState_ = State::done;
        resumeLater(recvWaiter_);
    }

    void requestStop() noexcept {
        if (stopQueued_) return;
        stopQueued_ = true;
        holdBegin();
        loop().schedule(co2::coroutine_handle<>::from_address(&stopFrame_.header));
    }

    static co2::detail::FrameHeader* onStopStep(co2::detail::FrameHeader* const header) {
        UdpBlock& self = *reinterpret_cast<StopFrame*>(header)->owner;
        self.stopQueued_ = false;
        // 被恢复的等待者可能已经再次 recv()：那就继续接收。
        if (self.receiving_ && self.recvState_ != State::pending && not self.closing()) {
            static_cast<void>(uv_udp_recv_stop(&self.udp_));
            self.receiving_ = false;
        }
        self.holdEnd(); // 最后一步：之后控制块可能已被释放
        return nullptr;
    }

    uv_udp_t udp_{};
    co2::coroutine_handle<> recvWaiter_;
    MutableBuffer recvBuf_;
    SocketAddress recvSender_;
    ssize_t recvStatus_ = 0;
    int recvError_ = 0;
    State recvState_ = State::idle;
    bool recvSuspended_ = false;
    bool recvTruncated_ = false;
    bool receiving_ = false;
    bool stopQueued_ = false;
    CancelHook recvCancel_;
    StopFrame stopFrame_;
};

class UdpRecvAwaiter {
  public:
    UdpRecvAwaiter(UdpBlock& block, MutableBuffer const buf) noexcept : block_{&block}, buf_{buf} {}

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return block_->suspendRecv(waiting, buf_, co2::detail::stopTokenOf(waiting));
    }

    Result<Datagram> await_resume() noexcept { return block_->resumeRecv(); }

  private:
    HandleRef<UdpBlock> block_;
    MutableBuffer buf_;
};

// send：快路径 uv_udp_try_send 发出时不分配；否则按需分配 uv_udp_send_t。
// 目标地址按值保存（sockaddr_in6 足以容纳 IPv4/IPv6），整个 awaiter 留在 co2 的内联槽里。
class UdpSendAwaiter {
  public:
    // to == nullptr：发往 connect() 的对端。
    UdpSendAwaiter(UdpBlock& block, ConstBuffer const data, SocketAddress const* const to) noexcept
        : block_{&block}, data_{data.data} {
        std::memset(&to_, 0, sizeof to_);
        if (data.size > maxDatagram) {
            status_ = UV_EMSGSIZE;
            size_ = 0;
        } else {
            size_ = static_cast<std::uint32_t>(data.size);
        }
        if (to != nullptr) {
            if (to->isIpv4())
                std::memcpy(&to_, to->raw(), sizeof(sockaddr_in));
            else if (to->isIpv6())
                std::memcpy(&to_, to->raw(), sizeof(sockaddr_in6));
            else if (status_ == 0)
                status_ = UV_EINVAL;
            hasTo_ = true;
        }
    }

    UdpSendAwaiter(UdpSendAwaiter&& other) noexcept
        : block_{other.block_}, data_{other.data_}, req_{nullptr}, to_(other.to_),
          size_{other.size_}, status_{other.status_}, hasTo_{other.hasTo_} {
        UVCO_CONTRACT_CHECK(other.req_ == nullptr && not other.started_);
    }
    UdpSendAwaiter(UdpSendAwaiter const&) = delete;
    UdpSendAwaiter& operator=(UdpSendAwaiter const&) = delete;
    UdpSendAwaiter& operator=(UdpSendAwaiter&&) = delete;

    ~UdpSendAwaiter() {
        UVCO_CONTRACT_CHECK(req_ == nullptr || not req_->pending); // 发送在途时销毁
        delete req_;
    }

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        UVCO_CONTRACT_CHECK(block_->loop().isLoopThread());
        UVCO_CONTRACT_CHECK(req_ == nullptr && not started_);
        if (co2::detail::stopTokenOf(waiting).stop_requested()) {
            status_ = UV_ECANCELED;
            return false;
        }
        if (status_ != 0) return false; // 构造时发现的参数错误
        if (block_->closing()) {
            status_ = UV_EBADF;
            return false;
        }
        return start(waiting);
    }

    Result<void> await_resume() noexcept {
        int const status = status_;
        delete std::exchange(req_, nullptr);
        if (std::exchange(started_, false)) block_->endOp(); // 最后访问控制块
        return fromStatus(status);
    }

  private:
    static constexpr std::size_t maxDatagram = 65535;

    struct Req {
        uv_udp_send_t req;
        co2::coroutine_handle<> waiter;
        UdpSendAwaiter* self;
        bool pending;
    };

    bool start(co2::coroutine_handle<> const waiting) {
        uv_buf_t buf;
        buf.base = const_cast<char*>(data_);
        buf.len = static_cast<decltype(buf.len)>(size_);
        sockaddr const* const to = hasTo_ ? reinterpret_cast<sockaddr const*>(&to_) : nullptr;
        uv_udp_t* const udp = block_->udp();

        int const tried = uv_udp_try_send(udp, &buf, 1, to);
        if (tried >= 0) { // 数据报整体发出
            status_ = 0;
            return false;
        }
        if (tried != UV_EAGAIN && tried != UV_ENOSYS) {
            status_ = tried;
            return false;
        }

        req_ = new Req{};
        req_->req.data = req_;
        req_->waiter = waiting;
        req_->self = this;
        int const status = uv_udp_send(&req_->req, udp, &buf, 1, to, &UdpSendAwaiter::onSend);
        if (status < 0) {
            delete std::exchange(req_, nullptr);
            status_ = status;
            return false;
        }
        req_->pending = true;
        started_ = true;
        block_->beginOp();
        return true;
    }

    static void onSend(uv_udp_send_t* const req, int const status) noexcept {
        Req* const r = static_cast<Req*>(req->data);
        r->pending = false;
        // send 不可取消：libuv 的 ECANCELED 只能来自 close。
        r->self->status_ = status == UV_ECANCELED ? UVCO_EABORTED : status;
        r->waiter.resume(); // libuv 回调里内联恢复（send 回调，不是 recv 回调）
    }

    HandleRef<UdpBlock> block_;
    char const* data_;
    Req* req_ = nullptr;
    sockaddr_in6 to_;
    std::uint32_t size_ = 0;
    int status_ = 0;
    bool hasTo_ = false;
    bool started_ = false;
};

static_assert(sizeof(UdpSendAwaiter) <= CO2_AWAIT_STORAGE_SIZE,
              "UdpSendAwaiter should fit co2's inline await slot");

} // namespace detail

class Udp : public detail::Handle<detail::UdpBlock> {
    using Base = detail::Handle<detail::UdpBlock>;

  public:
    // 空句柄（isOpen() == false），可作为帧局部变量的初值。
    Udp() noexcept = default;
    explicit Udp(Loop& loop) : Base{new detail::UdpBlock{loop}} {}

    // flags：UV_UDP_IPV6ONLY、UV_UDP_REUSEADDR 等。未绑定就 send/recv 时 libuv 自动绑定
    // 0.0.0.0:0。
    Result<void> bind(SocketAddress const& address, unsigned const flags = 0) noexcept {
        return fromStatus(uv_udp_bind(raw(), address.raw(), flags));
    }

    // 固定对端：之后 send(data) 发往它，且只收它的数据报。
    Result<void> connect(SocketAddress const& peer) noexcept {
        return fromStatus(uv_udp_connect(raw(), peer.raw()));
    }
    Result<void> disconnect() noexcept { return fromStatus(uv_udp_connect(raw(), nullptr)); }

    // 发往 to。数据必须保持有效直到 await 恢复；地址被复制。
    UVCO_NODISCARD detail::UdpSendAwaiter send(SocketAddress const& to,
                                               ConstBuffer const data) noexcept {
        return detail::UdpSendAwaiter{block(), data, &to};
    }
    // 发往 connect() 的对端。
    UVCO_NODISCARD detail::UdpSendAwaiter send(ConstBuffer const data) noexcept {
        return detail::UdpSendAwaiter{block(), data, nullptr};
    }

    // 收一个数据报。同一时间只能有一个 recv() 挂起。
    UVCO_NODISCARD detail::UdpRecvAwaiter recv(MutableBuffer const buf) noexcept {
        return detail::UdpRecvAwaiter{block(), buf};
    }

    Result<void> setBroadcast(bool const enable) noexcept {
        return fromStatus(uv_udp_set_broadcast(raw(), enable ? 1 : 0));
    }
    Result<void> setTtl(int const ttl) noexcept { return fromStatus(uv_udp_set_ttl(raw(), ttl)); }

    Result<SocketAddress> localAddress() const noexcept {
        sockaddr_storage storage;
        int length = static_cast<int>(sizeof storage);
        int const status =
            uv_udp_getsockname(raw(), reinterpret_cast<sockaddr*>(&storage), &length);
        if (status < 0) return static_cast<Errc>(status);
        return SocketAddress::fromRaw(reinterpret_cast<sockaddr const*>(&storage));
    }

    Result<SocketAddress> peerAddress() const noexcept {
        sockaddr_storage storage;
        int length = static_cast<int>(sizeof storage);
        int const status =
            uv_udp_getpeername(raw(), reinterpret_cast<sockaddr*>(&storage), &length);
        if (status < 0) return static_cast<Errc>(status);
        return SocketAddress::fromRaw(reinterpret_cast<sockaddr const*>(&storage));
    }

    // 底层 uv_udp_t（loop 线程上使用；不要 uv_close 它，也不要自行 recv_start/stop）。
    uv_udp_t* raw() const noexcept { return block().udp(); }
};

} // namespace uvco
