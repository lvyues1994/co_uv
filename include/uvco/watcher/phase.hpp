#pragma once
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include "uvco/detail/watcher/event_port.hpp"
#include "uvco/detail/watcher/native.hpp"
namespace uvco {
enum class Phase { idle, prepare, check };
struct PhaseEvent {
    Phase phase;
    std::uint64_t sequence; // per watcher, saturates; not a global loop-turn number
};
struct HookOptions { bool referenced = false; };
namespace detail {
struct IdleBackend {
    using Handle = uv_idle_t;
    static constexpr Phase phase = Phase::idle;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_idle_init(l, h); }
    static int start(Handle* h, uv_idle_cb cb) noexcept { return uv_idle_start(h, cb); }
    static int stop(Handle* h) noexcept { return uv_idle_stop(h); }
};
struct PrepareBackend {
    using Handle = uv_prepare_t;
    static constexpr Phase phase = Phase::prepare;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_prepare_init(l, h); }
    static int start(Handle* h, uv_prepare_cb cb) noexcept { return uv_prepare_start(h, cb); }
    static int stop(Handle* h) noexcept { return uv_prepare_stop(h); }
};
struct CheckBackend {
    using Handle = uv_check_t;
    static constexpr Phase phase = Phase::check;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_check_init(l, h); }
    static int start(Handle* h, uv_check_cb cb) noexcept { return uv_check_start(h, cb); }
    static int stop(Handle* h) noexcept { return uv_check_stop(h); }
};

template<class Backend, class Sink> class PhaseBlock final : public watcher::NativeWatcherBlock<Backend, Sink> {
    using Base = watcher::NativeWatcherBlock<Backend, Sink>;
  public:
    template<class... Args> explicit PhaseBlock(Loop& loop, Args&&... args)
        : Base{loop, std::forward<Args>(args)...} {}
    Result<void> start() noexcept {
        if (this->closing()) return Errc::aborted;
        return fromStatus(this->startNative(&PhaseBlock::fire)); // idempotent, sequence preserved
    }
    Result<void> stop() noexcept {
        if (this->closing()) return Errc::aborted;
        auto result = fromStatus(this->stopNative());
        if (result) this->stopDelivery();
        return result;
    }
  private:
    static void fire(typename Backend::Handle* native) noexcept {
        auto& self = Base::template fromData<PhaseBlock>(native->data);
        if (self.sequence_ != (std::numeric_limits<std::uint64_t>::max)()) ++self.sequence_;
        self.publish(PhaseEvent{Backend::phase, self.sequence_}); // no access after hook/resume
    }
    std::uint64_t sequence_ = 0;
};
using PhasePort = watcher::EventPort<PhaseEvent, watcher::LatestOnly<PhaseEvent>, watcher::DeferredNative>;
} // namespace detail
// Advanced observation only. A continuation is deferred, not executed precisely
// in the observed native phase. Prepare/Check do not guarantee poll wakeups.
template<class Backend> class PhaseWatcher : public detail::Handle<detail::PhaseBlock<Backend, detail::PhasePort>> {
    using Block = detail::PhaseBlock<Backend, detail::PhasePort>;
    using Base = detail::Handle<Block>;
  public:
    PhaseWatcher() noexcept = default;
    static Result<PhaseWatcher> create(Loop& loop, bool referenced = true) {
        Block* block = nullptr;
        try { block = new Block{loop}; } catch (std::bad_alloc const&) { return Errc::no_memory; }
        int const status = block->init();
        if (status < 0) { delete block; return static_cast<Errc>(status); }
        block->setReferenced(referenced);
        return PhaseWatcher{block};
    }
    Result<void> start() noexcept { return this->block().start(); }
    Result<void> stop() noexcept { return this->block().stop(); }
    bool isActive() const noexcept { return this->block().isActive(); }
    bool hasRef() const noexcept { return this->block().hasRef(); }
    void setReferenced(bool referenced) noexcept { this->block().setReferenced(referenced); }
    detail::watcher::DeliveryStats stats() const noexcept { return this->block().stats(); }
    UVCO_NODISCARD detail::watcher::NextAwaiter<Block> next() noexcept {
        return detail::watcher::NextAwaiter<Block>{this->block()};
    }
  private:
    explicit PhaseWatcher(Block* block) noexcept : Base{block} {}
};
using Idle = PhaseWatcher<detail::IdleBackend>;
using Prepare = PhaseWatcher<detail::PrepareBackend>;
using Check = PhaseWatcher<detail::CheckBackend>;

template<class Backend, class F> class PhaseHook : public detail::Handle<
    detail::PhaseBlock<Backend, detail::watcher::HookSink<PhaseEvent, F>>> {
    using Block = detail::PhaseBlock<Backend, detail::watcher::HookSink<PhaseEvent, F>>;
    using Base = detail::Handle<Block>;
  public:
    static Result<PhaseHook> create(Loop& loop, F callback, HookOptions options, bool autostart) {
        Block* block = nullptr;
        try { block = new Block{loop, std::move(callback)}; }
        catch (std::bad_alloc const&) { return Errc::no_memory; }
        int const status = block->init();
        if (status < 0) { delete block; return static_cast<Errc>(status); }
        PhaseHook hook{block}; block->setReferenced(options.referenced);
        if (autostart) { auto started = hook.start(); if (!started) return started.error(); }
        return Result<PhaseHook>{std::move(hook)};
    }
    Result<void> start() noexcept { return this->block().start(); }
    Result<void> stop() noexcept { return this->block().stop(); }
    bool isActive() const noexcept { return this->block().isActive(); }
    bool hasRef() const noexcept { return this->block().hasRef(); }
    void setReferenced(bool referenced) noexcept { this->block().setReferenced(referenced); }
  private:
    explicit PhaseHook(Block* block) noexcept : Base{block} {}
};
// No std::function/type-erasure allocation; callback is owned and must be noexcept.
template<class F> Result<PhaseHook<detail::PrepareBackend, F>>
onPrepare(Loop& loop, F callback, HookOptions options = {}) {
    return PhaseHook<detail::PrepareBackend, F>::create(loop, std::move(callback), options, true);
}
template<class F> Result<PhaseHook<detail::CheckBackend, F>>
onCheck(Loop& loop, F callback, HookOptions options = {}) {
    return PhaseHook<detail::CheckBackend, F>::create(loop, std::move(callback), options, true);
}
// Unlike prepare/check registration, Idle is created stopped: explicitly call
// start(). Active idle forces poll timeout=0 EVEN unref'ed: beware CPU spinning.
template<class F> Result<PhaseHook<detail::IdleBackend, F>>
onIdle(Loop& loop, F callback, HookOptions options = {}) {
    return PhaseHook<detail::IdleBackend, F>::create(loop, std::move(callback), options, false);
}
} // namespace uvco
