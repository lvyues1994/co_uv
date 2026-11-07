#include <array>
#include <memory>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/watcher/phase.hpp"
#include "uvco/core/run.hpp"
#include "uvco/sync/event.hpp"
#include "uvco/sync/timeout.hpp"
namespace {
auto wait(uvco::Loop& loop, uvco::Event& event)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop,event), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::timeout(loop,std::chrono::seconds{5},event.wait()));
    CO2_RETURN(r);
} CO2_END
auto next(uvco::Check& check)
    CO2_BEG(co2::Task<uvco::Result<uvco::PhaseEvent>>, (check), uvco::Result<uvco::PhaseEvent> r;) {
    CO2_AWAIT_SET(r,check.next()); CO2_RETURN(r);
} CO2_END
struct Frame {
    uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false};
    int count=0;
    static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* h) {
        ++reinterpret_cast<Frame*>(h)->count; return nullptr;
    }
    co2::coroutine_handle<> handle() { return co2::coroutine_handle<>::from_address(&header); }
};
struct Check : uvco::Check {
    explicit Check(uvco::Check&& c) : uvco::Check{std::move(c)} {}
    auto& impl() { return block(); }
};
}
TEST_CASE("unref hook does not keep an idle loop alive and idle hook starts explicitly") {
    uvco::Loop loop; int calls=0;
    auto hook=uvco::onPrepare(loop,[&](uvco::PhaseEvent) noexcept { ++calls; }); CHECK(hook);
    CHECK(!hook->hasRef()); CHECK(hook->isActive()); loop.runUntilIdle(); CHECK(calls==0);
    auto idle=uvco::onIdle(loop,[&](uvco::PhaseEvent) noexcept { ++calls; }); CHECK(idle);
    CHECK(!idle->isActive()); CHECK(!idle->hasRef()); CHECK(hook->stop());
}
TEST_CASE("controlled idle drives prepare then check without timer-order assumptions") {
    uvco::Loop loop; uvco::Event done{loop}; std::array<uvco::Phase,3> order{}; std::size_t n=0;
    bool i=false,p=false,c=false;
    auto idle=uvco::onIdle(loop,[&](uvco::PhaseEvent e) noexcept {
        if(!i){i=true; if(n<order.size()) order[n++]=e.phase;}
    });
    auto prepare=uvco::onPrepare(loop,[&](uvco::PhaseEvent e) noexcept {
        if(!p){p=true; if(n<order.size()) order[n++]=e.phase;}
    });
    auto check=uvco::onCheck(loop,[&](uvco::PhaseEvent e) noexcept {
        if(!c){c=true; if(n<order.size()) order[n++]=e.phase; done.set();}
    });
    CHECK(idle && prepare && check); CHECK(idle->start()); CHECK(uvco::run(loop,wait(loop,done)));
    CHECK(n==3); CHECK(order[0]==uvco::Phase::idle); CHECK(order[1]==uvco::Phase::prepare);
    CHECK(order[2]==uvco::Phase::check); CHECK(idle->stop()); CHECK(prepare->stop()); CHECK(check->stop());
}
TEST_CASE("advanced phase wait is deferred and stop does not overwrite committed completion") {
    uvco::Loop loop; auto made=uvco::Check::create(loop); CHECK(made); Check w{std::move(*made)};
    CHECK(w.hasRef()); CHECK(w.start()); Frame f;
    CHECK(w.impl().suspendNext(f.handle(),{})); static_cast<void>(uv_run(loop.raw(),UV_RUN_NOWAIT));
    CHECK(f.count==0); CHECK(w.stop()); loop.runUntilIdle(); CHECK(f.count==1);
    auto r=w.impl().resumeNext(); CHECK(r); CHECK(r->phase==uvco::Phase::check); CHECK(r->sequence==1);
}
TEST_CASE("phase stop aborts pending next later and pre-cancel leaves watcher active") {
    uvco::Loop loop; auto made=uvco::Check::create(loop); CHECK(made); Check w{std::move(*made)};
    CHECK(w.start()); Frame f; co2::stop_source stop; stop.request_stop();
    CHECK(!w.impl().suspendNext(f.handle(),stop.get_token()));
    CHECK(w.impl().resumeNext().error()==uvco::Errc::canceled); CHECK(w.isActive());
    CHECK(w.impl().suspendNext(f.handle(),{})); CHECK(w.stop()); CHECK(f.count==0);
    loop.runUntilIdle(); CHECK(f.count==1); CHECK(w.impl().resumeNext().error()==uvco::Errc::aborted);
}
TEST_CASE("Loop-first teardown drains phase awaiter before leaving safe orphan") {
    auto loop=std::unique_ptr<uvco::Loop>{new uvco::Loop}; auto made=uvco::Check::create(*loop); CHECK(made);
    CHECK(made->start()); auto task=next(*made); Frame done;
    uvco::detail::co2compat::TaskAccess::start(task,done.handle(),{}); CHECK(done.count==0);
    loop.reset(); CHECK(done.count==1);
    auto r=uvco::detail::co2compat::TaskAccess::takeResult(task); CHECK(!r); CHECK(r.error()==uvco::Errc::aborted);
    static_cast<void>(made->close());
}
UVCO_TEST_MAIN
