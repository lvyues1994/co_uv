#include "test.hpp"
#include "uvco/detail/watcher/native.hpp"
#include "uvco/detail/watcher/event_port.hpp"
namespace wd = uvco::detail::watcher;
namespace {
struct Backend {
    using Handle = uv_timer_t;
    static int init(uv_loop_t* l, Handle* h) noexcept { return uv_timer_init(l,h); }
    static int start(Handle* h, uv_timer_cb cb) noexcept { return uv_timer_start(h, cb, 0, 0); }
    static int stop(Handle* h) noexcept { return uv_timer_stop(h); }
};
using Port = wd::EventPort<int, wd::LatestOnly<int>>;
class Block final : public wd::NativeWatcherBlock<Backend, Port> {
    using Base = wd::NativeWatcherBlock<Backend, Port>;
  public:
    explicit Block(uvco::Loop& l) : Base{l} { CHECK(init()==0); }
    void emit(int value) noexcept { publish(value); }
    void projectionFailure() noexcept { publish(uvco::Errc::no_memory, std::make_exception_ptr(std::runtime_error("projection"))); }
    void start() { CHECK(startNative(&Block::fire)==0); }
    void stop() { CHECK(stopNative()==0); stopDelivery(); }
  private:
    static void fire(uv_timer_t* h) noexcept { fromData<Block>(h->data).emit(42); }
};
class Watch : public uvco::detail::Handle<Block> {
  public:
    explicit Watch(uvco::Loop& l) : Handle{new Block{l}} {}
    Block& impl() { return block(); }
    wd::NextAwaiter<Block> next() { return wd::NextAwaiter<Block>{block()}; }
};
struct Frame {
    uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false};
    int count=0;
    static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* h) {
        ++reinterpret_cast<Frame*>(h)->count; return nullptr;
    }
    co2::coroutine_handle<> handle() { return co2::coroutine_handle<>::from_address(&header); }
};
}
TEST_CASE("typed backend delivers final inactive callback inline") {
    uvco::Loop loop; Watch w{loop}; Frame f;
    w.impl().start(); CHECK(w.impl().suspendNext(f.handle(),{}));
    loop.runUntilIdle(); CHECK(f.count==1);
    auto result=w.impl().resumeNext(); CHECK(result); CHECK(*result==42);
}
TEST_CASE("pre-cancel does not consume buffered payload and projection exceptions drain") {
    uvco::Loop loop; Watch w{loop}; Frame f; co2::stop_source source; source.request_stop();
    w.impl().emit(7);
    CHECK(!w.impl().suspendNext(f.handle(),source.get_token()));
    CHECK(w.impl().resumeNext().error()==uvco::Errc::canceled);
    CHECK(!w.impl().suspendNext(f.handle(),{})); CHECK(*w.impl().resumeNext()==7);
    w.impl().projectionFailure(); CHECK(!w.impl().suspendNext(f.handle(),{}));
    bool threw=false; try { static_cast<void>(w.impl().resumeNext()); } catch(std::runtime_error const&) { threw=true; }
    CHECK(threw); CHECK(!w.impl().hasPendingOps());
}
TEST_CASE("close is deferred and preconstructed next retains old generation") {
    uvco::Loop loop; Watch w{loop}; Frame f;
    w.impl().start(); auto* p=&w.impl(); uvco::detail::HandleRef<Block> lease{p};
    CHECK(p->suspendNext(f.handle(),{})); static_cast<void>(w.close());
    CHECK(f.count==0); loop.runUntilIdle(); CHECK(f.count==1); CHECK(p->resumeNext().error()==uvco::Errc::aborted);
    CHECK(!p->suspendNext(f.handle(),{})); CHECK(p->resumeNext().error()==uvco::Errc::aborted);
}
UVCO_TEST_MAIN
