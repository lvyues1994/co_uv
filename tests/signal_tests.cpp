#include <atomic>
#include <chrono>
#include <csignal>
#include <limits>
#include <thread>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/process/process.hpp"
#include "uvco/core/run.hpp"
#include "uvco/sync/timeout.hpp"
#include "uvco/watcher/signal.hpp"
namespace {
struct Signal : uvco::Signal {
    explicit Signal(uvco::Signal&& signal) : uvco::Signal{std::move(signal)} {}
    uvco::detail::SignalBlock& impl() { return block(); }
};
struct Frame {
    uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false};
    int count=0;
    static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* h) {
        ++reinterpret_cast<Frame*>(h)->count; return nullptr;
    }
    co2::coroutine_handle<> handle() { return co2::coroutine_handle<>::from_address(&header); }
};
template<class P> void drive(uvco::Loop& loop, P pred) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
    while(!pred() && std::chrono::steady_clock::now()<deadline) {
        static_cast<void>(uv_run(loop.raw(),UV_RUN_NOWAIT)); std::this_thread::yield();
    }
    CHECK(pred());
}
}
TEST_CASE("signal validation rejects SIGPIPE and counting saturates") {
    uvco::Loop loop; auto made=uvco::Signal::create(loop); CHECK(made);
#ifdef SIGPIPE
    CHECK(!made->start(SIGPIPE));
#endif
    CHECK(!made->start(-1)); CHECK(!made->isActive());
#ifndef _WIN32
    CHECK(!made->start(SIGKILL)); CHECK(!made->start(SIGSTOP)); CHECK(!made->start(SIGSEGV));
#endif
    uvco::detail::SignalCount buffer;
    buffer.push({uvco::SignalEvent{1,(std::numeric_limits<std::uint64_t>::max)(),false},{}});
    buffer.push({uvco::SignalEvent{1,1,false},{}});
    auto record=buffer.pop(); CHECK(record.result->count==(std::numeric_limits<std::uint64_t>::max)());
    CHECK(record.result->saturated);
}
#ifndef _WIN32
TEST_CASE("signal notifications broadcast independently to two watchers") {
    uvco::Loop a; auto x=uvco::Signal::create(a),y=uvco::Signal::create(a); CHECK(x && y);
    Signal first{std::move(*x)}, second{std::move(*y)};
    CHECK(first.start(SIGUSR1)); CHECK(second.start(SIGUSR1));
    CHECK(raise(SIGUSR1)==0);
    drive(a,[&]{return first.queued()!=0;}); drive(a,[&]{return second.queued()!=0;});
    Frame f;
    CHECK(!first.impl().suspendNext(f.handle(),{})); auto r=first.impl().resumeNext();
    CHECK(r); CHECK(r->signum==SIGUSR1); CHECK(r->count>=1);
    CHECK(!second.impl().suspendNext(f.handle(),{})); CHECK(second.impl().resumeNext());
    CHECK(first.stop()); CHECK(second.stop());
}
TEST_CASE("signal broadcasts to a loop on another thread") {
    uvco::Loop loop; auto s=uvco::Signal::create(loop); CHECK(s); CHECK(s->start(SIGUSR1));
    std::atomic<bool> ready{false},seen{false}; std::exception_ptr exception;
    std::thread thread{[&] {
        try {
            uvco::Loop other; auto watcher=uvco::Signal::create(other); CHECK(watcher);
            CHECK(watcher->start(SIGUSR1)); ready.store(true,std::memory_order_release);
            drive(other,[&]{return watcher->queued()!=0;}); seen.store(true,std::memory_order_release);
            CHECK(watcher->stop());
        } catch(...) { exception=std::current_exception(); ready.store(true,std::memory_order_release); }
    }};
    struct Join {std::thread& t; ~Join(){if(t.joinable())t.join();}} join{thread};
    drive(loop,[&]{return ready.load(std::memory_order_acquire);});
    CHECK(raise(SIGUSR1)==0); drive(loop,[&]{return s->queued()!=0;}); thread.join();
    if(exception) std::rethrow_exception(exception);
    CHECK(seen.load(std::memory_order_acquire)); CHECK(s->stop());
}
namespace {
auto childExit(uvco::Loop& loop, uvco::Process& process)
    CO2_BEG(co2::Task<uvco::Result<uvco::ExitStatus>>, (loop,process), uvco::Result<uvco::ExitStatus> r;) {
    CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::seconds{5},process.wait()));
    CO2_RETURN(r);
} CO2_END
}
TEST_CASE("SIGCHLD observer never steals Process reaping") {
    uvco::Loop loop; auto signal=uvco::Signal::create(loop); CHECK(signal); CHECK(signal->start(SIGCHLD));
    uvco::ProcessOptions options; options.file=UVCO_TEST_PROCESS_HELPER; options.args={"exit","17"};
    auto child=uvco::spawn(loop,options); CHECK(child);
    auto status=uvco::run(loop,childExit(loop,*child)); CHECK(status); CHECK(status->exit_status==17);
    drive(loop,[&]{return signal->queued()!=0;}); CHECK(signal->stop());
}
TEST_CASE("signal cancel only cancels waiter and restart aborts the old generation later") {
    uvco::Loop loop; auto made=uvco::Signal::create(loop); CHECK(made); Signal s{std::move(*made)};
    CHECK(s.start(SIGUSR1)); Frame f; co2::stop_source source;
    CHECK(s.impl().suspendNext(f.handle(),source.get_token())); source.request_stop();
    drive(loop,[&]{return f.count==1;}); CHECK(s.impl().resumeNext().error()==uvco::Errc::canceled);
    CHECK(s.isActive());
    auto* old=&s.impl(); uvco::detail::HandleRef<uvco::detail::SignalBlock> lease{old};
    CHECK(old->suspendNext(f.handle(),{})); CHECK(!s.start(SIGPIPE)); CHECK(f.count==1);
    CHECK(s.start(SIGUSR2)); CHECK(f.count==1);
    drive(loop,[&]{return f.count==2;}); CHECK(old->resumeNext().error()==uvco::Errc::aborted);
    CHECK(s.impl().suspendNext(f.handle(),{})); auto* current=&s.impl();
    uvco::detail::HandleRef<uvco::detail::SignalBlock> keep{current}; static_cast<void>(s.close());
    drive(loop,[&]{return f.count==3;}); CHECK(current->resumeNext().error()==uvco::Errc::aborted);
}
#endif
UVCO_TEST_MAIN
