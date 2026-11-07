#pragma once

#include <cstring>
#include <new>
#include <string>
#include <utility>
#include "uvco/detail/libuv.hpp"
#include "uvco/fs/detail/watch.hpp"

namespace uvco {
namespace fs {

struct FsEventInfo {
    std::string filename; // owned relative name; empty when libuv supplies NULL
    int events = 0;       // UV_RENAME | UV_CHANGE, not an operation journal
};

namespace detail {
struct FsEventMerge {
    bool operator()(FsEventInfo& tail, FsEventInfo const& incoming) const noexcept {
        if (tail.filename != incoming.filename) return false;
        tail.events |= incoming.events;
        return true;
    }
};

class FsEventBlock final : public WatchBlock<FsEventInfo, FsEventMerge> {
  public:
    FsEventBlock(Loop& loop, int& status) noexcept : WatchBlock{loop} {
        status = uv_fs_event_init(loop.raw(), &event_);
        if (status == 0) bindHandle(); // ENOSYS leaves no native handle to close
    }
    ~FsEventBlock() override = default;
    Result<void> start(char const* path, unsigned int flags) noexcept {
        return fromStatus(uv_fs_event_start(&event_, &FsEventBlock::onEvent, path, flags));
    }
    Result<void> stop() noexcept {
        if (closing()) return Errc::aborted;
        auto result = fromStatus(uv_fs_event_stop(&event_));
        if (result) abortPending();
        return result;
    }
  private:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&event_); }
    static void onEvent(uv_fs_event_t* handle, char const* filename, int events, int status) noexcept {
        auto& self = fromData<FsEventBlock>(handle->data);
        if (status < 0) {
            self.publish(static_cast<Errc>(status));
            return;
        }
        Result<FsEventInfo> result = Errc::no_memory;
        std::exception_ptr exception;
        try {
            FsEventInfo info;
            if (filename) info.filename.append(filename, std::strlen(filename));
            info.events = events;
            result = std::move(info);
        } catch (std::bad_alloc const&) {
            // C callbacks must not throw. Native errors and ENOMEM are Result errors.
        } catch (...) {
            exception = std::current_exception();
        }
        self.publish(std::move(result), std::move(exception)); // no self access after resume
    }
    uv_fs_event_t event_{};
};

inline Result<void> checkFsEventFlags(unsigned int flags) noexcept {
    unsigned int const known = UV_FS_EVENT_WATCH_ENTRY | UV_FS_EVENT_STAT | UV_FS_EVENT_RECURSIVE;
    if ((flags & ~known) != 0) return Errc::invalid_argument;
    // libuv currently implements neither flag on any backend. Do not silently
    // pretend that STAT selects FsPoll or that WATCH_ENTRY is implemented.
    if ((flags & (UV_FS_EVENT_WATCH_ENTRY | UV_FS_EVENT_STAT)) != 0)
        return static_cast<Errc>(UV_ENOTSUP);
#if !defined(_WIN32) && !defined(__APPLE__)
    if ((flags & UV_FS_EVENT_RECURSIVE) != 0) return static_cast<Errc>(UV_ENOTSUP);
#endif
    return {};
}
} // namespace detail

// Fallible factory: some backends (e.g. QNX) return ENOSYS at *init*, before
// uv_close is legal. Empty/moved/closed handles have the usual Handle contract.
class FsEvent : public ::uvco::detail::Handle<detail::FsEventBlock> {
    using Base = ::uvco::detail::Handle<detail::FsEventBlock>;
  public:
    using Info = FsEventInfo;
    FsEvent() noexcept = default;

    static Result<FsEvent> watch(Loop& loop, std::string path, unsigned int flags = 0) {
        UVCO_CONTRACT_CHECK(loop.isLoopThread());
        auto checked = detail::checkFsEventFlags(flags);
        if (!checked) return checked.error();
        if (path.find('\0') != std::string::npos) return Errc::invalid_argument;
        int status = 0;
        detail::FsEventBlock* block = nullptr;
        try { block = new detail::FsEventBlock{loop, status}; }
        catch (std::bad_alloc const&) { return Errc::no_memory; }
        if (status < 0) {
            delete block; // never uv_close an uninitialized handle
            return static_cast<Errc>(status);
        }
        FsEvent watcher{block};
        auto started = block->start(path.c_str(), flags); // libuv owns its path copy
        if (!started) return started.error(); // watcher closes initialized handle
        return Result<FsEvent>{std::move(watcher)};
    }

    // Transactional replacement: failed restart keeps the old watch; successful
    // restart discards its queue/counters and aborts an old pending next later.
    Result<void> watch(std::string path, unsigned int flags = 0) {
        auto replacement = FsEvent::watch(loop(), std::move(path), flags);
        if (!replacement) return replacement.error();
        static_cast<void>(close());
        Base::operator=(std::move(*replacement));
        return {};
    }
    Result<void> stop() noexcept { return block().stop(); }
    bool isActive() const noexcept { return block().isActive(); }
    UVCO_NODISCARD detail::WatchAwaiter<detail::FsEventBlock> next() noexcept { return detail::WatchAwaiter<detail::FsEventBlock>{block()}; }
    static constexpr std::size_t bufferCapacity() noexcept { return detail::FsEventBlock::bufferCapacity(); }
    std::size_t queued() const noexcept { return block().queued(); }
    std::uint64_t overflowCount() const noexcept { return block().overflowCount(); }
    std::uint64_t coalescedCount() const noexcept { return block().coalescedCount(); }
  private:
    explicit FsEvent(detail::FsEventBlock* block) noexcept : Base{block} {}
};

} // namespace fs
} // namespace uvco
