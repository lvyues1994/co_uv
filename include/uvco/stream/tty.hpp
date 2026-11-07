#pragma once
#include "uvco/detail/libuv.hpp"
#include <cerrno>
#include <limits>
#include <new>
#include <utility>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "uvco/stream/stream.hpp"
namespace uvco {
enum class TtyMode { Normal, Raw, Io };
struct TerminalSize { int width = 0; int height = 0; };
namespace detail {
inline void closeTtyFd(uv_file fd) noexcept {
#ifdef _WIN32
    static_cast<void>(_close(fd));
#else
    static_cast<void>(::close(fd)); // do not retry close(EINTR): fd can already be reused
#endif
}
struct TtyFdGuard {
    uv_file fd;
    explicit TtyFdGuard(uv_file value) noexcept : fd{value} {}
    TtyFdGuard(TtyFdGuard const&) = delete;
    ~TtyFdGuard() { if (fd >= 0) closeTtyFd(fd); }
    void release() noexcept { fd = -1; }
};
inline Result<uv_file> duplicateTtyFd(uv_file fd) noexcept {
    if (fd < 0) return Errc::bad_descriptor;
#ifdef _WIN32
    if (uv_get_osfhandle(fd) == INVALID_HANDLE_VALUE) return Errc::bad_descriptor;
    // uv_tty_init duplicates HANDLEs for CRT slots 0..2, but consumes CRT slots
    // >2. Reserve low free slots temporarily so failure never enters libuv's
    // internal stdio-duplication branch (whose failure cleanup varies by version).
    int low[3]; int size = 0; int copy;
    for (;;) {
        copy = _dup(fd);
        if (copy < 0 || copy > 2) break;
        low[size++] = copy;
    }
    int const saved = errno;
    while (size != 0) closeTtyFd(low[--size]);
    if (copy < 0) return static_cast<Errc>(uv_translate_sys_error(saved));
    if (!SetHandleInformation(uv_get_osfhandle(copy), HANDLE_FLAG_INHERIT, 0)) {
        auto error = static_cast<Errc>(uv_translate_sys_error(static_cast<int>(GetLastError())));
        closeTtyFd(copy); return error;
    }
    return copy;
#else
    int copy;
#ifdef F_DUPFD_CLOEXEC
    do { copy = fcntl(fd, F_DUPFD_CLOEXEC, 3); } while (copy < 0 && errno == EINTR);
    if (copy >= 0 || errno != EINVAL) {
        if (copy < 0) return static_cast<Errc>(uv_translate_sys_error(errno));
        return copy;
    }
#endif
    do { copy = fcntl(fd, F_DUPFD, 3); } while (copy < 0 && errno == EINTR);
    if (copy < 0) return static_cast<Errc>(uv_translate_sys_error(errno));
    if (fcntl(copy, F_SETFD, FD_CLOEXEC) < 0) {
        auto error = static_cast<Errc>(uv_translate_sys_error(errno)); closeTtyFd(copy); return error;
    }
    return copy;
#endif
}
class TtyBlock final : public StreamBlock {
  public:
    explicit TtyBlock(Loop& loop) noexcept : StreamBlock{loop} {}
    ~TtyBlock() override = default;
    int init(uv_file fd) noexcept {
        int const status = uv_tty_init(loop().raw(), &tty_, fd, 0);
        if (status == 0) bindHandle();
        return status;
    }
    uv_tty_t* tty() noexcept { return &tty_; }
    Result<void> setMode(TtyMode mode) noexcept {
        if (closing()) return Errc::bad_descriptor;
        if (hasReadReservation()) return Errc::busy;
        if (uv_is_readable(stream()) == 0) return static_cast<Errc>(UV_ENOTSUP);
        uv_tty_mode_t native;
        switch (mode) {
            case TtyMode::Normal: native = UV_TTY_MODE_NORMAL; break;
            case TtyMode::Raw: native = UV_TTY_MODE_RAW; break;
            case TtyMode::Io: native = UV_TTY_MODE_IO; break;
            default: return Errc::invalid_argument;
        }
        int const status = uv_tty_set_mode(&tty_, native);
        if (status == 0) mode_ = mode;
        return fromStatus(status);
    }
  private:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&tty_); }
    StreamBlock* newPeer() override { UVCO_CONTRACT_FAIL("TTY has no accept peers"); }
    void restoreForClose() noexcept {
        if (closing() || mode_ == TtyMode::Normal) return;
        // Unlike explicit setMode, teardown can own a pending read. Stop native
        // reading first; closeInternally then aborts the unchanged WaitSlot later.
        static_cast<void>(uv_read_stop(stream()));
        if (uv_tty_set_mode(&tty_, UV_TTY_MODE_NORMAL) == 0) mode_ = TtyMode::Normal;
    }
    void beginClose() noexcept override { restoreForClose(); closeInternally(); }
    void loopClosing() noexcept override { restoreForClose(); closeInternally(); }
    uv_tty_t tty_{};
    TtyMode mode_ = TtyMode::Normal;
};
} // namespace detail
// Duplicate-only ownership: caller's fd remains open on every path. Unix may
// reopen the tty; fallback dup and all console modes can share external state.
// Do not concurrently mutate terminal modes through another owner. No implicit
// process-global uv_tty_reset_mode, no promise to undo arbitrary foreign changes.
class Tty : public detail::Handle<detail::TtyBlock> {
    using Base = detail::Handle<detail::TtyBlock>;
  public:
    Tty() noexcept = default;
    static Result<Tty> duplicate(Loop& loop, uv_file fd) {
        UVCO_CONTRACT_CHECK(loop.isLoopThread());
        auto copied = detail::duplicateTtyFd(fd);
        if (!copied) return copied.error();
        detail::TtyFdGuard duplicate{*copied};
        if (uv_guess_handle(duplicate.fd) != UV_TTY) return static_cast<Errc>(UV_ENOTTY);
        detail::TtyBlock* block = nullptr;
        try { block = new detail::TtyBlock{loop}; } catch (std::bad_alloc const&) { return Errc::no_memory; }
        int const status = block->init(duplicate.fd);
        if (status < 0) { delete block; return static_cast<Errc>(status); }
        Tty owner{block};
        uv_os_fd_t native;
        // A successful uv_tty_init is bound and has a valid public uv_fileno.
        int const named = uv_fileno(reinterpret_cast<uv_handle_t*>(block->tty()), &native);
        UVCO_CONTRACT_CHECK(named == 0);
#ifdef _WIN32
        bool const consumed = native == uv_get_osfhandle(duplicate.fd);
#else
        bool const consumed = native == duplicate.fd;
#endif
        // When libuv reopens, native owns a different fd and the auxiliary
        // duplicate must close now. Fallback/native-consumed fd closes via uv_close.
        if (consumed) duplicate.release();
        return Result<Tty>{std::move(owner)};
    }
    UVCO_NODISCARD detail::ReadAwaiter read(MutableBuffer buf) noexcept { return detail::ReadAwaiter{block(), buf}; }
    UVCO_NODISCARD detail::WriteAwaiter write(ConstBuffer buf) noexcept { return detail::WriteAwaiter{block(), buf}; }
    UVCO_NODISCARD detail::WriteAwaiter write(std::vector<ConstBuffer> bufs) noexcept {
        return detail::WriteAwaiter{block(), std::move(bufs)};
    }
    Result<std::size_t> tryWrite(ConstBuffer buf) noexcept {
        if (block().closing()) return Errc::bad_descriptor;
        if (buf.size == 0) return std::size_t{0};
        if (!buf.data || buf.size > static_cast<std::size_t>((std::numeric_limits<detail::UvBufLen>::max)()))
            return Errc::invalid_argument;
        auto native = detail::makeUvBuf(buf.data, buf.size);
        int const n = uv_try_write(block().stream(), &native, 1);
        if (n < 0) return static_cast<Errc>(n);
        return static_cast<std::size_t>(n);
    }
    UVCO_NODISCARD detail::ShutdownAwaiter shutdown() noexcept { return detail::ShutdownAwaiter{block()}; }
    Result<void> setMode(TtyMode mode) noexcept { return block().setMode(mode); }
    Result<void> restore() noexcept { return setMode(TtyMode::Normal); }
    Result<TerminalSize> getSize() const noexcept {
        if (block().closing()) return Errc::bad_descriptor;
        TerminalSize size;
        int const status = uv_tty_get_winsize(raw(), &size.width, &size.height);
        if (status < 0) return static_cast<Errc>(status);
        return size;
    }
    bool isReadable() const noexcept { return uv_is_readable(block().stream()) != 0; }
    bool isWritable() const noexcept { return uv_is_writable(block().stream()) != 0; }
    std::size_t writeQueueSize() const noexcept { return uv_stream_get_write_queue_size(block().stream()); }
    uv_tty_t* raw() const noexcept { return block().tty(); }
  private:
    explicit Tty(detail::TtyBlock* block) noexcept : Base{block} {}
};
} // namespace uvco
