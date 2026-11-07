#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include "uvco/detail/libuv.hpp"
#include "uvco/fs/detail/watch.hpp"
#include "uvco/detail/watcher/native.hpp"

namespace uvco {
namespace fs {

struct FsPollChange {
    uv_stat_t prev{};
    uv_stat_t curr{};
};

namespace detail {
struct FsPollBackend {
    using Handle = uv_fs_poll_t;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_fs_poll_init(l, h); }
    static int start(Handle* h, uv_fs_poll_cb cb, char const* path, unsigned int interval) noexcept {
        return uv_fs_poll_start(h, cb, path, interval);
    }
    static int stop(Handle* h) noexcept { return uv_fs_poll_stop(h); }
};
class FsPollBlock final : public ::uvco::detail::watcher::NativeWatcherBlock<FsPollBackend, WatchBlock<FsPollChange>> {
    using Base = ::uvco::detail::watcher::NativeWatcherBlock<FsPollBackend, WatchBlock<FsPollChange>>;
  public:
    FsPollBlock(Loop& loop, int& status, std::string path, unsigned int interval) noexcept
        : Base{loop}, path_{std::move(path)}, interval_{interval} {
        status = init();
    }
    ~FsPollBlock() override = default;
    Result<void> start() noexcept {
        return fromStatus(startNative(&FsPollBlock::onPoll, path_.c_str(), interval_));
    }
    Result<void> stop() noexcept {
        if (closing()) return Errc::aborted;
        auto result = fromStatus(stopNative());
        if (result) abortPending();
        return result;
    }
    std::string const& path() const noexcept { return path_; }
    std::chrono::milliseconds interval() const noexcept {
        return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(interval_)};
    }
  private:
    static void onPoll(uv_fs_poll_t* handle, int status,
                       uv_stat_t const* prev, uv_stat_t const* curr) noexcept {
        auto& self = fromData<FsPollBlock>(handle->data);
        if (status < 0) {
            // prev/curr are not useful on errors; the watcher remains active.
            self.publish(static_cast<Errc>(status));
            return;
        }
        UVCO_CONTRACT_CHECK(prev != nullptr && curr != nullptr);
        self.publish(FsPollChange{*prev, *curr}); // copy before callback storage expires
    }
    std::string path_; // owned for setInterval/restart; never borrow caller strings
    unsigned int interval_;
};
} // namespace detail

class FsPoll : public ::uvco::detail::Handle<detail::FsPollBlock> {
    using Base = ::uvco::detail::Handle<detail::FsPollBlock>;
  public:
    using Info = FsPollChange;
    FsPoll() noexcept = default;

    static Result<FsPoll> start(Loop& loop, std::string path,
                                std::chrono::milliseconds interval = std::chrono::milliseconds{1000}) {
        UVCO_CONTRACT_CHECK(loop.isLoopThread());
        if (interval.count() <= 0 ||
            static_cast<std::uint64_t>(interval.count()) >
                static_cast<std::uint64_t>((std::numeric_limits<unsigned int>::max)()) ||
            path.find('\0') != std::string::npos) return Errc::invalid_argument;
        int status = 0;
        detail::FsPollBlock* block = nullptr;
        try {
            block = new detail::FsPollBlock{loop, status, std::move(path),
                                            static_cast<unsigned int>(interval.count())};
        } catch (std::bad_alloc const&) { return Errc::no_memory; }
        if (status < 0) {
            delete block; // native init failed: not a closable handle
            return static_cast<Errc>(status);
        }
        FsPoll watcher{block};
        auto started = block->start();
        if (!started) return started.error();
        return Result<FsPoll>{std::move(watcher)};
    }

    // Never stop/start the *same* uv_fs_poll_t: on supported older libuv, an
    // old stat callback can observe the restarted handle active and revive its
    // stale context. A fresh block also preserves leases for old next awaiters.
    // Failure leaves the old watcher untouched; success aborts old next later.
    Result<void> start(std::string path,
                        std::chrono::milliseconds interval = std::chrono::milliseconds{1000}) {
        auto replacement = FsPoll::start(loop(), std::move(path), interval);
        if (!replacement) return replacement.error();
        static_cast<void>(close());
        Base::operator=(std::move(*replacement));
        return {};
    }
    Result<void> setInterval(std::chrono::milliseconds interval) {
        return start(block().path(), interval);
    }
    std::chrono::milliseconds interval() const noexcept { return block().interval(); }
    Result<void> stop() noexcept { return block().stop(); }
    bool isActive() const noexcept { return block().isActive(); }
    UVCO_NODISCARD detail::WatchAwaiter<detail::FsPollBlock> next() noexcept {
        return detail::WatchAwaiter<detail::FsPollBlock>{block()};
    }
    static constexpr std::size_t bufferCapacity() noexcept { return detail::FsPollBlock::bufferCapacity(); }
    std::size_t queued() const noexcept { return block().queued(); }
    std::uint64_t overflowCount() const noexcept { return block().overflowCount(); }
    std::uint64_t coalescedCount() const noexcept { return block().coalescedCount(); }
  private:
    explicit FsPoll(detail::FsPollBlock* block) noexcept : Base{block} {}
};

} // namespace fs
} // namespace uvco
