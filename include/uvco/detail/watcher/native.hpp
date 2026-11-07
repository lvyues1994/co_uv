#pragma once
#include <type_traits>
#include <utility>
#include "uvco/core/handle.hpp"
#include "uvco/core/result.hpp"
namespace uvco { namespace detail { namespace watcher {
// Backend owns only typed libuv calls. Sink owns delivery/lifetime accounting.
// No union, type switch, virtual public interface or per-event heap allocation.
template<class Backend, class Sink> class NativeWatcherBlock : public Sink {
  public:
    using Native = typename Backend::Handle;
    template<class... Args> explicit NativeWatcherBlock(Loop& loop, Args&&... args)
        : Sink{loop, std::forward<Args>(args)...} {}
    ~NativeWatcherBlock() override = default;
    // Init failure: caller deletes this block directly; never uv_close it.
    int init() noexcept {
        int status = Backend::init(this->loop().raw(), &native_);
        if (status == 0) this->bindHandle();
        return status;
    }
    Native* raw() noexcept { return &native_; }
  protected:
    template<class... Args> int startNative(Args&&... args) noexcept {
        return Backend::start(&native_, std::forward<Args>(args)...);
    }
    int stopNative() noexcept { return Backend::stop(&native_); }
  private:
    uv_handle_t* uvHandle() noexcept override { return reinterpret_cast<uv_handle_t*>(&native_); }
    Native native_{};
};
// Hook callbacks are owned, synchronous and statically noexcept. Unlike an
// EventPort this sink has no waiter, cancellation hook, queue or coroutine frame.
template<class E, class F> class HookSink : public HandleBlock {
    static_assert(noexcept(std::declval<F&>()(std::declval<E const&>())),
                  "phase hook callback must be noexcept");
  public:
    using Info = E;
  protected:
    HookSink(Loop& loop, F callback) : HandleBlock{loop}, callback_{std::move(callback)} {}
    void publish(E const& event) noexcept {
        if (!closing()) callback_(event); // no this access after callback
    }
    void abortPending() noexcept override {}
  private:
    F callback_;
};
} } } // namespace uvco::detail::watcher
