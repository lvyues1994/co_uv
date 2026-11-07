#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/stream.hpp"

// Pipe：Unix 域套接字 / Windows 命名管道（docs/architecture.md §4.6），只可移动。
// 不支持 IPC（传递句柄）。
//
//   uvco::Pipe server{loop};
//   server.bind("/tmp/app.sock");  server.listen();
//   CO2_AWAIT_SET(client, server.accept());   // Result<Pipe>
//
//   uvco::Pipe pipe{loop};
//   CO2_AWAIT_SET(r, pipe.connect("/tmp/app.sock"));
//
// 取消 connect 与 Tcp 相同：整个 Pipe 被关闭。

namespace uvco {

class Pipe;

namespace detail {

class PipeBlock final : public StreamBlock {
  public:
    explicit PipeBlock(Loop& loop) : StreamBlock{loop} {
        int const status = uv_pipe_init(loop.raw(), &pipe_, 0);
        UVCO_CONTRACT_CHECK(status == 0);
        bindHandle();
    }

    uv_pipe_t* pipe() noexcept { return &pipe_; }

  protected:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&pipe_); }
    StreamBlock* newPeer() override { return new PipeBlock{loop()}; }

  private:
    uv_pipe_t pipe_{};
};

class PipeConnectAwaiter final : public ConnectAwaiterBase<PipeConnectAwaiter> {
  public:
    PipeConnectAwaiter(PipeBlock& block, std::string path)
        : ConnectAwaiterBase{block}, path_{std::move(path)} {}

    int startOp(uv_connect_t* const req) noexcept {
        // uv_pipe_connect 的错误总是经回调（下一次迭代）报告。
        uv_pipe_connect(req, static_cast<PipeBlock*>(static_cast<StreamBlock*>(block_))->pipe(), path_.c_str(),
                        &PipeConnectAwaiter::onDone);
        return 0;
    }

  private:
    std::string path_;
};

using PipeNameFn = int (*)(uv_pipe_t const*, char*, std::size_t*);

inline Result<std::string> pipeName(uv_pipe_t const* const pipe, PipeNameFn const fn) {
    std::string name(256, '\0');
    std::size_t size = name.size();
    int status = fn(pipe, &name[0], &size);
    if (status == UV_ENOBUFS) {
        name.assign(size + 1, '\0');
        size = name.size();
        status = fn(pipe, &name[0], &size);
    }
    if (status < 0) return static_cast<Errc>(status);
    name.resize(size);
    return name;
}

} // namespace detail

class Pipe : public detail::StreamHandle<detail::PipeBlock, Pipe> {
    using Base = detail::StreamHandle<detail::PipeBlock, Pipe>;

  public:
    Pipe() noexcept = default; // 空句柄
    explicit Pipe(Loop& loop) : Base{new detail::PipeBlock{loop}} {}

    // Ownership transfers only on success. On failure caller retains fd;
    // native open may still have changed descriptor flags (e.g. nonblocking).
    static Result<Pipe> open(Loop& loop, uv_file fd) {
        Pipe stream{loop};
        int const status=uv_pipe_open(stream.raw(),fd);
        if(status<0)return static_cast<Errc>(status);
        return Result<Pipe>{std::move(stream)};
    }

    Result<void> bind(std::string const& path) noexcept {
        return fromStatus(uv_pipe_bind(raw(), path.c_str()));
    }

    UVCO_NODISCARD detail::PipeConnectAwaiter connect(std::string path) {
        return detail::PipeConnectAwaiter{block(), std::move(path)};
    }

    Result<std::string> localName() const { return detail::pipeName(raw(), &uv_pipe_getsockname); }
    Result<std::string> peerName() const { return detail::pipeName(raw(), &uv_pipe_getpeername); }

    uv_pipe_t* raw() const noexcept { return block().pipe(); }

  private:
    explicit Pipe(detail::PipeBlock* const block) noexcept : Base{block} {}
    template <class, class> friend class detail::AcceptAwaiter;
};

} // namespace uvco
