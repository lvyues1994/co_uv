#include <type_traits>
#include "test.hpp"
#include "uvco/timer/timer.hpp"
static_assert(std::is_same<decltype(std::declval<uvco::Timer&>().next()), uvco::detail::TickAwaiter>::value,
              "next is an exact alias, not another timer API");
namespace {
struct InspectTimer : uvco::Timer {
    explicit InspectTimer(uvco::Loop& loop) : Timer{loop} {}
    uvco::detail::TimerBlock& impl() { return block(); }
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
TEST_CASE("Timer start keeps a pending tick and native completion stays inline") {
    uvco::Loop loop; InspectTimer timer{loop}; Frame f;
    CHECK(timer.start(std::chrono::milliseconds{10000}));
    CHECK(timer.impl().suspendTick(f.handle(),{}));
    CHECK(timer.start(std::chrono::milliseconds{0})); CHECK(f.count==0);
    loop.runUntilIdle(); CHECK(f.count==1); CHECK(*timer.impl().resumeTick()==1);
}
TEST_CASE("Timer post-close lease preserves cache and legacy result priority") {
    uvco::Loop loop; InspectTimer timer{loop}; Frame f;
    CHECK(timer.start(std::chrono::milliseconds{0})); loop.runUntilIdle();
    auto* p=&timer.impl(); uvco::detail::HandleRef<uvco::detail::TimerBlock> lease{p};
    static_cast<void>(timer.close()); loop.runUntilIdle();
    co2::stop_source stop; stop.request_stop();
    CHECK(!p->suspendTick(f.handle(),stop.get_token())); CHECK(p->resumeTick().error()==uvco::Errc::canceled);
    CHECK(!p->suspendTick(f.handle(),{})); CHECK(*p->resumeTick()==1);
    CHECK(!p->suspendTick(f.handle(),{})); CHECK(p->resumeTick().error()==uvco::Errc::invalid_argument);
}
UVCO_TEST_MAIN
