#include <co2/co2.hpp>
#include <chrono>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
uvco::ProcessOptions command(std::string mode,std::string arg={}){uvco::ProcessOptions p;p.file=UVCO_TEST_PROCESS_HELPER;p.args={std::move(mode),std::move(arg)};return p;}
auto timed(uvco::Loop& loop,uvco::CaptureCancel action)
 CO2_BEG(co2::Task<uvco::Result<uvco::CaptureResult>>,(loop,action),uvco::CaptureOptions io;uvco::Result<uvco::CaptureResult> r;) {
 io.cancel=action;io.input=std::string(1048576,'x');CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::milliseconds{0},uvco::capture(loop,command("sleep","0.1"),std::move(io))));CO2_RETURN(std::move(r));
} CO2_END
}
TEST_CASE("communicate concurrently drains both outputs while writing stdin"){
 uvco::Loop loop;uvco::CaptureOptions io;io.input=std::string(131072,'i');io.stdoutLimit=400000;io.stderrLimit=200000;
 auto r=uvco::run(loop,uvco::communicate(loop,command("duplex"),std::move(io)));CHECK(r);CHECK(r->status.exitStatus==0);
 CHECK(r->stdoutData==std::string(131072,'o')+std::string(131072,'i'));CHECK(r->stderrData==std::string(131072,'e'));
}
TEST_CASE("capture rejects oversized output"){
 uvco::Loop loop;uvco::CaptureOptions io;io.stdoutLimit=4;auto r=uvco::run(loop,uvco::capture(loop,command("echo","oversized"),std::move(io)));
 CHECK(!r);CHECK(r.error()==static_cast<uvco::Errc>(UV_ENOBUFS));loop.runUntilIdle();
}
TEST_CASE("capture cancellation options drain IO without changing default reap"){
 for(auto mode:{uvco::CaptureCancel::stopWaiting,uvco::CaptureCancel::killChild}){
  uvco::Loop loop;auto r=uvco::run(loop,timed(loop,mode));CHECK(!r);CHECK(r.error()==uvco::Errc::timed_out);loop.runUntilIdle();
 }
#ifndef _WIN32
 uvco::Loop loop;auto r=uvco::run(loop,timed(loop,uvco::CaptureCancel::killGroup));CHECK(!r);CHECK(r.error()==uvco::Errc::timed_out);loop.runUntilIdle();
#endif
}
TEST_CASE("capture spawn error stays a Result"){
 uvco::Loop loop;uvco::ProcessOptions p;p.file="/uvco-no-such-program";auto r=uvco::run(loop,uvco::capture(loop,std::move(p)));CHECK(!r);
}

TEST_CASE("capture stderr cap is independent of stdout cap"){
 uvco::Loop loop;uvco::CaptureOptions io;io.stderrLimit=4;
 auto r=uvco::run(loop,uvco::capture(loop,command("duplex"),std::move(io)));CHECK(!r);CHECK(r.error()==static_cast<uvco::Errc>(UV_ENOBUFS));loop.runUntilIdle();
}
TEST_CASE("capture direct-tail caps preserve exact limit and zero-cap EOF") {
 for(auto cap:{std::size_t{0},std::size_t{4096}}){
  uvco::Loop loop;uvco::CaptureOptions io;io.stdoutLimit=cap;io.stderrLimit=0;
  auto p=cap==0?command("exit","0"):command("echo",std::string(4095,'x'));
  auto r=uvco::run(loop,uvco::capture(loop,std::move(p),std::move(io)));CHECK(r);CHECK(r->stdoutData.size()==cap);
 }
 {uvco::Loop loop;uvco::CaptureOptions io;io.stdoutLimit=0;auto r=uvco::run(loop,uvco::capture(loop,command("echo",""),std::move(io)));CHECK(!r);CHECK(r.error()==static_cast<uvco::Errc>(UV_ENOBUFS));}
}
UVCO_TEST_MAIN
