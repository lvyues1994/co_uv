#pragma once

#include "uvco/core/co2_compat.hpp"

#include <climits>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include "uvco/detail/libuv.hpp"

#include "uvco/core/cancel.hpp"
#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/wait_slot.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/req_awaiter.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/buffer.hpp"

// 流（docs/architecture.md §4.6）：Tcp、Pipe 的公共部分。
//
//   read(buf)  -> Result<std::size_t>   拉模式：只在有读者挂起时 uv_read_start，读到一块
//                                       就 uv_read_stop——没人读时数据留在内核缓冲区里，
//                                       对端自然被反压。libuv 直接读进用户缓冲区（零拷贝）。
//                                       对端关闭写端后返回 Errc::eof（此后一直如此）。
//                                       同一时刻只能有一个读者（契约）。可取消。
//   write(buf) / write(bufs) -> Result<void>
//                                       先 uv_try_write；没写完的部分 uv_write 排队，全部交给
//                                       内核后完成。多个 write 可以并发，按调用顺序排队。
//                                       挂起时 stop_token 取消会关闭整个流并返回 canceled；close() 会让它以
//                                       Errc::aborted 结束；出错时不知道写出了多少字节。
//   shutdown() -> Result<void>          等排队的写完成后关闭写端。不可取消。
//   listen(backlog) / accept()          accept 拉模式：没有 accept 挂起时 libuv 保留一个
//                                       待接受的连接并暂停监听 fd（连接留在内核 backlog）。
//                                       同一时刻只能有一个 accept（契约）。可取消。
//
// 所有等待都遵守先停止规则：停止已请求时立即 ECANCELED，即使操作本可以立即完成。
// close()（或 Loop 析构）时挂起中的 read/accept/write/shutdown/connect 以 Errc::aborted
// 结束——与 stop_token 取消（Errc::canceled）区分。
// 在已关闭的流（例如 connect 被取消后）上操作返回 Errc::bad_descriptor。

namespace uvco {
namespace detail {

using UvBufLen = decltype(uv_buf_t{}.len);

inline uv_buf_t makeUvBuf(char const* const data, std::size_t const size) noexcept {
    uv_buf_t b;
    b.base = const_cast<char*>(data);
    b.len = static_cast<UvBufLen>(size);
    return b;
}

class StreamBlock : public HandleBlock {
  public:
    uv_stream_t* stream() noexcept { return reinterpret_cast<uv_stream_t*>(uvHandle()); }

    // 供块外的 awaiter（write/shutdown/connect）记账：挂起时 beginOp，恢复时最后一步 endOp。
    void beginOp() noexcept { opBegin(); }
    void endOp() noexcept { opEnd(); }

    bool hasReadReservation() const noexcept { return !readSlot_.idle(); }

    // ---- read ----

    bool suspendRead(co2::coroutine_handle<> const waiter, MutableBuffer const buf,
                     co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        if (not readSlot_.idle())
            UVCO_CONTRACT_FAIL("uvco stream: concurrent read() on the same stream");
        UVCO_CONTRACT_CHECK(buf.data != nullptr && buf.size != 0);
        readSlot_.begin();
        if (token.stop_requested()) {
            readSlot_.set(static_cast<Errc>(static_cast<int>(UV_ECANCELED)));
            return false;
        }
        if (closing()) {
            readSlot_.set(static_cast<Errc>(static_cast<int>(UV_EBADF)));
            return false;
        }
        if (readEnd_ != 0) { // EOF / 错误是粘性的
            readSlot_.set(static_cast<Errc>(static_cast<int>(readEnd_)));
            return false;
        }
        int const status = uv_read_start(stream(), &StreamBlock::onAlloc, &StreamBlock::onRead);
        if (status < 0) {
            readSlot_.set(static_cast<Errc>(static_cast<int>(status)));
            return false;
        }
        readBuf_ = buf;
        readSlot_.pend(waiter);
        opBegin();
        readCancel_.arm(loop(), std::move(token), &StreamBlock::onReadCancel, this);
        return true;
    }

    Result<std::size_t> resumeRead() noexcept {
        readCancel_.disarm();
        auto taken = readSlot_.take();
        readBuf_ = MutableBuffer{};
        if (taken.suspended) opEnd();
        return std::move(taken.result);
    }

    // ---- listen / accept ----

    int listen(int const backlog) noexcept {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        if (closing()) return UV_EBADF;
        int const status = uv_listen(stream(), backlog, &StreamBlock::onConnection);
        if (status == 0) listening_ = true;
        return status;
    }

    bool suspendAccept(co2::coroutine_handle<> const waiter, co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());
        if (not acceptSlot_.idle())
            UVCO_CONTRACT_FAIL("uvco stream: concurrent accept() on the same stream");
        acceptSlot_.begin();
        if (token.stop_requested()) {
            acceptSlot_.set(acceptResult(UV_ECANCELED));
            return false;
        }
        if (closing()) {
            acceptSlot_.set(acceptResult(UV_EBADF));
            return false;
        }
        if (not listening_) {
            acceptSlot_.set(acceptResult(UV_EINVAL));
            return false;
        }
        if (listenError_ != 0) {
            acceptSlot_.set(acceptResult(std::exchange(listenError_, 0)));
            return false;
        }
        int const status = tryAccept();
        if (status != UV_EAGAIN) {
            acceptSlot_.set(acceptResult(status));
            return false;
        }
        acceptSlot_.pend(waiter);
        opBegin();
        acceptCancel_.arm(loop(), std::move(token), &StreamBlock::onAcceptCancel, this);
        return true;
    }

    // 成功时返回新连接的控制块（调用者接管所有权）。
    Result<StreamBlock*> resumeAccept() noexcept {
        acceptCancel_.disarm();
        auto taken = acceptSlot_.take();
        if (taken.suspended) opEnd();
        return std::move(taken.result);
    }

  protected:
    explicit StreamBlock(Loop& loop) noexcept : HandleBlock{loop} {}
    ~StreamBlock() override = default;

    // 新建一个同类型、已 init、未连接的控制块，作为 uv_accept 的目标。
    virtual StreamBlock* newPeer() = 0;

    void abortPending() noexcept override {
        // uv_close 已经停止读和监听。
        if (readSlot_.pending()) finishRead(UVCO_EABORTED, Resume::later);
        if (acceptSlot_.pending()) finishAccept(UVCO_EABORTED, Resume::later);
        if (peer_ != nullptr) static_cast<void>(std::exchange(peer_, nullptr)->release({}));
    }

  private:
    enum class Resume : unsigned char { inlineNow, later };

    static constexpr std::size_t maxBufLen() noexcept {
        return static_cast<std::size_t>((std::numeric_limits<UvBufLen>::max)()) <
                       (std::numeric_limits<std::size_t>::max)()
                   ? static_cast<std::size_t>((std::numeric_limits<UvBufLen>::max)())
                   : (std::numeric_limits<std::size_t>::max)();
    }

    static void onAlloc(uv_handle_t* const handle, std::size_t, uv_buf_t* const buf) noexcept {
        StreamBlock& self = fromData<StreamBlock>(handle->data);
        if (self.readSlot_.pending()) {
            std::size_t const size =
                self.readBuf_.size < maxBufLen() ? self.readBuf_.size : maxBufLen();
            *buf = makeUvBuf(self.readBuf_.data, size); // 直接读进读者的缓冲区
        } else {
            // 没有读者（只是防御：读者离开时总会 uv_read_stop）：不取走数据，libuv 以
            // UV_ENOBUFS 回调，onRead 停止读取，数据留在内核里。
            *buf = makeUvBuf(nullptr, 0);
        }
    }

    static void onRead(uv_stream_t* const stream, ssize_t const nread, uv_buf_t const*) noexcept {
        StreamBlock& self = fromData<StreamBlock>(stream->data);
        if (nread == 0) return; // EAGAIN：继续等
        static_cast<void>(uv_read_stop(stream));
        if (not self.readSlot_.pending()) {
            UVCO_CONTRACT_CHECK(nread < 0); // 没有交出过缓冲区，不可能读到数据
            if (nread != UV_ENOBUFS) self.readEnd_ = nread;
            return;
        }
        if (nread < 0) self.readEnd_ = nread; // UV_EOF 或错误：粘性
        self.finishRead(nread, Resume::inlineNow);
    }

    static void onReadCancel(void* const context) noexcept {
        auto& self = *static_cast<StreamBlock*>(context);
        if (not self.readSlot_.pending()) return;
        static_cast<void>(uv_read_stop(self.stream()));
        self.finishRead(UV_ECANCELED, Resume::inlineNow);
    }

    void finishRead(ssize_t const status, Resume const how) noexcept {
        Result<std::size_t> result = status < 0
            ? Result<std::size_t>{static_cast<Errc>(static_cast<int>(status))}
            : Result<std::size_t>{static_cast<std::size_t>(status)};
        auto const waiter = readSlot_.complete(std::move(result));
        if (how == Resume::inlineNow)
            waiter.resume(); // 之后不再访问 this
        else
            resumeLater(waiter);
    }

    // 0：accepted_ 已就绪；UV_EAGAIN：没有待接受的连接；其他：错误。
    int tryAccept() {
        if (peer_ == nullptr) peer_ = newPeer(); // 留到下一次 accept 复用
        int const status = uv_accept(stream(), peer_->stream());
        if (status == 0) accepted_ = std::exchange(peer_, nullptr);
        return status;
    }

    static void onConnection(uv_stream_t* const server, int const status) noexcept {
        StreamBlock& self = fromData<StreamBlock>(server->data);
        if (status < 0) {
            if (self.acceptSlot_.pending())
                self.finishAccept(status, Resume::inlineNow);
            else
                self.listenError_ = status;
            return;
        }
        // 没有 accept 挂起：libuv 保留这个连接，暂停监听直到下一次 uv_accept。
        if (not self.acceptSlot_.pending()) return;
        int const accepted = self.tryAccept();
        if (accepted == UV_EAGAIN) return;
        self.finishAccept(accepted, Resume::inlineNow);
    }

    static void onAcceptCancel(void* const context) noexcept {
        auto& self = *static_cast<StreamBlock*>(context);
        if (not self.acceptSlot_.pending()) return;
        self.finishAccept(UV_ECANCELED, Resume::inlineNow);
    }

    Result<StreamBlock*> acceptResult(int status) noexcept {
        return status < 0 ? Result<StreamBlock*>{static_cast<Errc>(status)}
                          : Result<StreamBlock*>{std::exchange(accepted_, nullptr)};
    }
    void finishAccept(int const status, Resume const how) noexcept {
        auto const waiter = acceptSlot_.complete(acceptResult(status));
        if (how == Resume::inlineNow)
            waiter.resume();
        else
            resumeLater(waiter);
    }

    // read
    WaitSlot<std::size_t> readSlot_;
    MutableBuffer readBuf_;
    ssize_t readEnd_ = 0; // 0，或粘性的 UV_EOF / 错误码
    CancelHook readCancel_;

    // accept
    WaitSlot<StreamBlock*> acceptSlot_;
    StreamBlock* peer_ = nullptr;     // 预先创建的 accept 目标
    StreamBlock* accepted_ = nullptr; // 已接受、等待交给 accept() 的连接
    int listenError_ = 0;
    bool listening_ = false;
    CancelHook acceptCancel_;
};

class ReadAwaiter {
  public:
    ReadAwaiter(StreamBlock& block, MutableBuffer const buf) noexcept : block_{&block}, buf_{buf} {}

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return block_->suspendRead(waiting, buf_, ::uvco::detail::co2compat::stopTokenOf(waiting));
    }

    Result<std::size_t> await_resume() noexcept { return block_->resumeRead(); }

  private:
    HandleRef<StreamBlock> block_;
    MutableBuffer buf_;
};

template <class Self, class Block> class AcceptAwaiter {
  public:
    explicit AcceptAwaiter(Block& block) noexcept : block_{&block} {}

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        return block_->suspendAccept(waiting, ::uvco::detail::co2compat::stopTokenOf(waiting));
    }

    Result<Self> await_resume() {
        Result<StreamBlock*> r = block_->resumeAccept();
        if (not r) return r.error();
        return Self{static_cast<Block*>(*r)};
    }

  private:
    HandleRef<Block> block_;
};

// write：快路径 uv_try_write 一次写完时不分配；否则按需分配 uv_write_t。
// 大小控制在 co2 的内联槽（8 个指针）以内。
class WriteAwaiter {
  public:
    WriteAwaiter(StreamBlock& block, ConstBuffer const one) noexcept
        : block_{&block}, one_{one}, single_{true} {}
    WriteAwaiter(StreamBlock& block, std::vector<ConstBuffer> many) noexcept
        : block_{&block}, many_{std::move(many)}, single_{false} {}

    WriteAwaiter(WriteAwaiter&& other) noexcept
        : block_{other.block_}, one_{other.one_}, many_{std::move(other.many_)},
          single_{other.single_} {
        UVCO_CONTRACT_CHECK(other.req_ == nullptr && not other.started_);
    }
    WriteAwaiter(WriteAwaiter const&) = delete;
    WriteAwaiter& operator=(WriteAwaiter const&) = delete;
    WriteAwaiter& operator=(WriteAwaiter&&) = delete;

    ~WriteAwaiter() {
        // 写在途时销毁：libuv 仍持有请求。
        UVCO_CONTRACT_CHECK(req_ == nullptr || not req_->pending);
        delete req_;
    }

    bool await_ready() const noexcept { return false; }

    template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
        UVCO_CONTRACT_CHECK(block_->loop().isLoopThread());
        UVCO_CONTRACT_CHECK(req_ == nullptr && not started_);
        if (::uvco::detail::co2compat::stopTokenOf(waiting).stop_requested()) {
            status_ = UV_ECANCELED;
            return false;
        }
        if (block_->closing()) {
            status_ = UV_EBADF;
            return false;
        }
        bool const pending = start(waiting);
        if (pending) cancel_.arm(block_->loop(), ::uvco::detail::co2compat::stopTokenOf(waiting), &WriteAwaiter::onCancel, this);
        return pending;
    }

    Result<void> await_resume() noexcept {
        cancel_.disarm();
        int const status = status_;
        delete std::exchange(req_, nullptr);
        if (std::exchange(started_, false)) block_->endOp(); // 最后访问控制块
        return fromStatus(status);
    }

  private:
    struct Req {
        uv_write_t req;
        co2::coroutine_handle<> waiter;
        WriteAwaiter* self;
        bool pending;
    };

    static constexpr std::size_t inlineBufs = 8;

    bool start(co2::coroutine_handle<> const waiting) {
        ConstBuffer const* const in = single_ ? &one_ : many_.data();
        std::size_t const count = single_ ? 1 : many_.size();

        uv_buf_t small[inlineBufs];
        std::vector<uv_buf_t> big;
        uv_buf_t* bufs = small;
        if (count > inlineBufs) {
            big.resize(count);
            bufs = big.data();
        }
        std::size_t total = 0;
        unsigned n = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (in[i].size == 0) continue;
            if (static_cast<unsigned long long>(in[i].size) >
                    static_cast<unsigned long long>((std::numeric_limits<UvBufLen>::max)()) ||
                n == UINT_MAX) {
                status_ = UV_EINVAL;
                return false;
            }
            bufs[n++] = makeUvBuf(in[i].data, in[i].size);
            total += in[i].size;
        }
        if (n == 0) { // 什么也不用写
            status_ = 0;
            return false;
        }

        uv_stream_t* const stream = block_->stream();
        int const tried = uv_try_write(stream, bufs, n);
        if (tried < 0 && tried != UV_EAGAIN) {
            status_ = tried;
            return false;
        }
        std::size_t written = tried > 0 ? static_cast<std::size_t>(tried) : 0;
        if (written == total) {
            status_ = 0;
            return false;
        }
        unsigned first = 0;
        while (written >= bufs[first].len) {
            written -= bufs[first].len;
            ++first;
        }
        bufs[first].base += written;
        bufs[first].len = static_cast<UvBufLen>(bufs[first].len - written);

        try { req_ = new Req{}; }
        catch (...) { if (tried > 0) block_->closeInternally(); throw; }
        req_->req.data = req_;
        req_->waiter = waiting;
        req_->self = this;
        int const status =
            uv_write(&req_->req, stream, bufs + first, n - first, &WriteAwaiter::onWrite);
        if (status < 0) {
            delete std::exchange(req_, nullptr);
            status_ = status;
            if (tried > 0) block_->closeInternally();
            return false;
        }
        req_->pending = true;
        started_ = true;
        block_->beginOp();
        return true;
    }

    static void onCancel(void* context) noexcept {
        auto& self = *static_cast<WriteAwaiter*>(context);
        if (!self.req_ || !self.req_->pending || self.block_->closing()) return;
        self.canceled_ = true;
        self.block_->closeInternally(); // wait for onWrite before releasing user buffers
    }
    static void onWrite(uv_write_t* const req, int const status) noexcept {
        Req* const r = static_cast<Req*>(req->data);
        r->pending = false;
        r->self->status_ = r->self->canceled_ ? UV_ECANCELED : (status == UV_ECANCELED ? UVCO_EABORTED : status);
        if (status < 0) r->self->block_->closeInternally(); // queued write may have emitted a prefix
        r->waiter.resume(); // libuv 回调里内联恢复
    }

    HandleRef<StreamBlock> block_;
    ConstBuffer one_;
    std::vector<ConstBuffer> many_;
    Req* req_ = nullptr;
    int status_ = 0;
    bool single_;
    bool started_ = false;
    bool canceled_ = false;
    CancelHook cancel_;
};

// shutdown / connect：基于 ReqAwaiter（请求体在 awaiter 里，帧局部或 co2 槽）。
template <class Derived, class Req> class StreamReqAwaiter : public ReqAwaiter<Derived, Req, void> {
    using Base = ReqAwaiter<Derived, Req, void>;

  public:
    StreamReqAwaiter(StreamReqAwaiter&&) = default;

    int start(uv_loop_t*) noexcept {
        if (block_->closing()) return UV_EBADF;
        int const status = static_cast<Derived&>(*this).startOp(&this->req_);
        if (status == 0) {
            started_ = true;
            block_->beginOp();
        }
        return status;
    }

    Result<void> finish(int const status) noexcept {
        bool const began = std::exchange(started_, false);
        bool const requested = std::exchange(cancelRequested_, false);
        if (began) block_->endOp(); // 最后访问控制块
        // 请求发起后 libuv 的 ECANCELED 来自 close：除非它正是 stop_token 取消（connect）
        // 引起的，否则报告为 Errc::aborted。（发起前的先停止规则仍是 ECANCELED。）
        if (began && status == UV_ECANCELED && not requested) return fromStatus(UVCO_EABORTED);
        return fromStatus(status);
    }

  protected:
    explicit StreamReqAwaiter(StreamBlock& block) noexcept : Base{block.loop()}, block_{&block} {}

    static void onDone(Req* const req, int const status) noexcept { Base::complete(req, status); }

    HandleRef<StreamBlock> block_;
    bool started_ = false;
    bool cancelRequested_ = false;
};

class ShutdownAwaiter final : public StreamReqAwaiter<ShutdownAwaiter, uv_shutdown_t> {
  public:
    static constexpr bool cancellable = false; // libuv 无法取消 shutdown 请求

    explicit ShutdownAwaiter(StreamBlock& block) noexcept : StreamReqAwaiter{block} {}

    int startOp(uv_shutdown_t* const req) noexcept {
        return uv_shutdown(req, block_->stream(), &ShutdownAwaiter::onDone);
    }
};

// connect 的公共部分：libuv 无法取消连接请求，取消时关闭整个流（连接回调随后以
// UV_ECANCELED 触发，报告为 Errc::canceled）；之后这个流只能 close()/析构。
template <class Derived>
class ConnectAwaiterBase : public StreamReqAwaiter<Derived, uv_connect_t> {
  public:
    ConnectAwaiterBase(ConnectAwaiterBase&&) = default;
    void cancel() noexcept {
        this->cancelRequested_ = true;
        this->block_->closeInternally();
    }

  protected:
    using StreamReqAwaiter<Derived, uv_connect_t>::StreamReqAwaiter;
};

// Tcp / Pipe 共用的用户接口。
template <class Block, class Self> class StreamHandle : public Handle<Block> {
    using Base = Handle<Block>;

  public:
    UVCO_NODISCARD ReadAwaiter read(MutableBuffer const buf) noexcept {
        return ReadAwaiter{this->block(), buf};
    }

    UVCO_NODISCARD WriteAwaiter write(ConstBuffer const buf) noexcept {
        return WriteAwaiter{this->block(), buf};
    }
    UVCO_NODISCARD WriteAwaiter write(std::vector<ConstBuffer> bufs) noexcept {
        return WriteAwaiter{this->block(), std::move(bufs)};
    }

    UVCO_NODISCARD ShutdownAwaiter shutdown() noexcept { return ShutdownAwaiter{this->block()}; }

    Result<void> listen(int const backlog = 128) noexcept {
        return fromStatus(this->block().listen(backlog));
    }

    UVCO_NODISCARD AcceptAwaiter<Self, Block> accept() noexcept {
        return AcceptAwaiter<Self, Block>{this->block()};
    }

    // libuv 中排队等待写出的字节数。
    std::size_t writeQueueSize() const noexcept {
        return uv_stream_get_write_queue_size(this->block().stream());
    }
    bool isReadable() const noexcept { return uv_is_readable(this->block().stream()) != 0; }
    bool isWritable() const noexcept { return uv_is_writable(this->block().stream()) != 0; }

  protected:
    StreamHandle() noexcept = default;
    explicit StreamHandle(Block* const block) noexcept : Base{block} {}
};

} // namespace detail
} // namespace uvco
