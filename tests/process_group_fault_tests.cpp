#include <cerrno>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/process/capture.hpp"
#include "uvco/core/run.hpp"
namespace {int groupCalls=0;bool reapedWindow=false;}
extern "C" int __real_waitid(idtype_t,id_t,siginfo_t*,int);
extern "C" int __wrap_waitid(idtype_t type,id_t id,siginfo_t* info,int flags) {
 if(reapedWindow){errno=ECHILD;return -1;}return __real_waitid(type,id,info,flags);
}
extern "C" int __real_uv_kill(int,int);
extern "C" int __wrap_uv_kill(int pid,int signal) {
 if(pid<0){++groupCalls;return 0;}return __real_uv_kill(pid,signal);
}
extern "C" int __real_kill(pid_t,int);
extern "C" int __wrap_kill(pid_t pid,int signal) {
 if(pid<0){++groupCalls;return 0;}return __real_kill(pid,signal);
}
namespace {
auto wait(uvco::Process& p) CO2_BEG(co2::Task<>,(p),uvco::Result<uvco::ExitStatus> status;) {
 CO2_AWAIT_SET(status,p.wait());CHECK(status);
} CO2_END
}
TEST_CASE("capture group cancel rejects libuv reap-before-exit-callback window") {
 uvco::Loop loop;uvco::CaptureOptions io;io.cancel=uvco::CaptureCancel::killGroup;
 uvco::detail::CaptureState state{loop,io};uvco::ProcessOptions options;
 options.file=UVCO_TEST_PROCESS_HELPER;options.args={"sleep","30"};options.privateGroup=true;
 auto child=uvco::spawn(loop,options);CHECK(child);state.proc=std::move(*child);
 reapedWindow=true;state.interrupt();reapedWindow=false;
 int observed=groupCalls;CHECK(state.proc.kill(SIGTERM));uvco::run(loop,wait(state.proc));
 CHECK(observed==0);
}
UVCO_TEST_MAIN
