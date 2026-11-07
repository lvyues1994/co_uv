#pragma once
#include <csignal>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include "uvco/detail/watcher/event_port.hpp"
#include "uvco/detail/watcher/native.hpp"
namespace uvco {
// Counts libuv callbacks, not the number of POSIX signals sent (both layers can
// coalesce). Each watcher receives its own notifications, even across loops.
struct SignalEvent {
    int signum = 0;
    std::uint64_t count = 0;
    bool saturated = false;
};
namespace detail {
struct SignalMerge {
    bool operator()(SignalEvent& tail, SignalEvent const& incoming) const noexcept {
        if (tail.signum != incoming.signum) return false;
        auto const max = (std::numeric_limits<std::uint64_t>::max)();
        if (incoming.count > max - tail.count) { tail.count = max; tail.saturated = true; }
        else tail.count += incoming.count;
        tail.saturated = tail.saturated || incoming.saturated;
        return true;
    }
};
using SignalCount = watcher::BoundedFifo<SignalEvent, 1, SignalMerge>;
struct SignalBackend {
    using Handle = uv_signal_t;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_signal_init(l, h); }
    static int start(Handle* h, uv_signal_cb cb, int signum) noexcept { return uv_signal_start(h, cb, signum); }
    static int stop(Handle* h) noexcept { return uv_signal_stop(h); }
};
using SignalPort = watcher::EventPort<SignalEvent, SignalCount>;
class SignalBlock final : public watcher::NativeWatcherBlock<SignalBackend, SignalPort> {
    using Base = watcher::NativeWatcherBlock<SignalBackend, SignalPort>;
  public:
    explicit SignalBlock(Loop& loop) : Base{loop} {}
    Result<void> startSignal(int signum) noexcept { return fromStatus(startNative(&SignalBlock::fire, signum)); }
    Result<void> stop() noexcept {
        if (closing()) return Errc::aborted;
        auto result = fromStatus(stopNative());
        if (result) stopDelivery();
        return result;
    }
  private:
    static void fire(uv_signal_t* h, int signum) noexcept {
        fromData<SignalBlock>(h->data).publish(SignalEvent{signum, 1, false});
    }
};
inline Result<void> validateSignal(int signum) noexcept {
    if (signum <= 0) return Errc::invalid_argument;
#ifdef SIGPIPE
    if (signum == SIGPIPE) return Errc::invalid_argument; // preserve Loop's SIGPIPE policy
#endif
#ifdef _WIN32
    if (signum == SIGINT) return {};
#ifdef SIGBREAK
    if (signum == SIGBREAK) return {};
#endif
#ifdef SIGHUP
    if (signum == SIGHUP) return {};
#endif
#ifdef SIGWINCH
    if (signum == SIGWINCH) return {};
#endif
    return static_cast<Errc>(UV_ENOTSUP);
#else
#ifdef NSIG
    if (signum >= NSIG) return Errc::invalid_argument;
#endif
    if (signum == SIGKILL || signum == SIGSTOP || signum == SIGBUS ||
        signum == SIGFPE || signum == SIGILL || signum == SIGSEGV) return Errc::invalid_argument;
#if defined(__linux__)
    if (signum >= 32 && signum < SIGRTMIN) return Errc::invalid_argument; // NPTL reserved
#endif
    return {};
#endif
}
} // namespace detail
// Dispositions are process-global. Do not mix with foreign sigaction owners.
// SIGCHLD is notification-only: Process/libuv remain the sole child reapers.
class Signal : public detail::Handle<detail::SignalBlock> {
    using Base = detail::Handle<detail::SignalBlock>;
  public:
    Signal() noexcept = default;
    static Result<Signal> create(Loop& loop) {
        detail::SignalBlock* block = nullptr;
        try { block = new detail::SignalBlock{loop}; }
        catch (std::bad_alloc const&) { return Errc::no_memory; }
        int const status = block->init();
        if (status < 0) { delete block; return static_cast<Errc>(status); }
        return Signal{block};
    }
    // Every successful start creates a fresh epoch, including the same signum.
    // Failure preserves the old watch; old next leases never switch generations.
    Result<void> start(int signum) {
        auto& owner = loop();
        auto checked = detail::validateSignal(signum);
        if (!checked) return checked.error();
        auto replacement = create(owner);
        if (!replacement) return replacement.error();
        auto started = replacement->block().startSignal(signum);
        if (!started) return started.error();
        static_cast<void>(close());
        Base::operator=(std::move(*replacement));
        return {};
    }
    Result<void> stop() noexcept { return block().stop(); }
    bool isActive() const noexcept { return block().isActive(); }
    std::size_t queued() const noexcept { return block().queued(); }
    std::uint64_t coalescedCount() const noexcept { return block().coalescedCount(); }
    UVCO_NODISCARD detail::watcher::NextAwaiter<detail::SignalBlock> next() noexcept {
        return detail::watcher::NextAwaiter<detail::SignalBlock>{block()};
    }
  private:
    explicit Signal(detail::SignalBlock* block) noexcept : Base{block} {}
};
} // namespace uvco
