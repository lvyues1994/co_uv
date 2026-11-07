#include <csignal>
#include <cerrno>
#include <chrono>
#include <thread>
#include <memory>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/process.hpp"
#include "uvco/uvco.hpp"
namespace {
auto waitFor(uvco::Process& p)
 CO2_BEG(co2::Task<uvco::Result<uvco::ExitStatus>>, (p), uvco::Result<uvco::ExitStatus> r;) {
 CO2_AWAIT_SET(r,p.wait()); CO2_RETURN(r);
} CO2_END
}
TEST_CASE("process exit codes") {
 uvco::Loop loop;
 for(int code : {0, 1, 42}) {
  uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER; o.args={"exit", std::to_string(code)};
  auto p=uvco::spawn(loop,o); CHECK(p);
  auto r=uvco::run(loop,waitFor(*p)); CHECK(r && r->exitStatus==code && r->termSignal==0);
 }
}
namespace {
auto exchange(uvco::Process& p, uvco::Pipe& in, uvco::Pipe& out)
 CO2_BEG(co2::Task<std::string>, (p,in,out), uvco::Result<void> w;
         uvco::Result<std::size_t> n; std::string text; char buf[64]; uvco::Result<uvco::ExitStatus> status;) {
 CO2_AWAIT_SET(w,in.write(uvco::ConstBuffer{"hello cat\n",10})); CHECK(w);
 CO2_AWAIT_SET(w,in.shutdown()); CHECK(w);
 for (;;) {
  CO2_AWAIT_SET(n,out.read(uvco::buffer(buf)));
  if (not n) { CHECK(n.error()==uvco::Errc::eof); break; }
  text.append(buf,*n);
 }
 CO2_AWAIT_SET(status,p.wait()); CHECK(status && status->exitStatus==0);
 CO2_RETURN(text);
} CO2_END

auto output(uvco::Process& p, uvco::Pipe& out)
 CO2_BEG(co2::Task<std::string>, (p,out), uvco::Result<std::size_t> n;
         std::string text; char buf[64]; uvco::Result<uvco::ExitStatus> status;) {
 for (;;) {
  CO2_AWAIT_SET(n,out.read(uvco::buffer(buf)));
  if (not n) { CHECK(n.error()==uvco::Errc::eof); break; }
  text.append(buf,*n);
 }
 CO2_AWAIT_SET(status,p.wait()); CHECK(status && status->exitStatus==0);
 CO2_RETURN(text);
} CO2_END

std::size_t fds() {
 DIR* d=opendir("/proc/self/fd"); CHECK(d); std::size_t n=0;
 while(readdir(d)) ++n;
 closedir(d); return n;
}
uvco::ProcessOptions sleeper() {
 uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER; o.args={"sleep","30"}; return o;
}
}
TEST_CASE("process stdout echo pipe") {
 uvco::Loop loop; uvco::Pipe out{loop};
 uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER; o.args={"echo","hello"};
 o.stdio={uvco::ProcessStdio::ignore(),uvco::ProcessStdio::createPipe(out,false,true)};
 auto p=uvco::spawn(loop,o); CHECK(p);
 CHECK(uvco::run(loop,output(*p,out))=="hello\n");
}
TEST_CASE("process stdin cat roundtrip") {
 uvco::Loop loop; uvco::Pipe in{loop},out{loop};
 uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER;o.args={"cat"};
 o.stdio={uvco::ProcessStdio::createPipe(in,true,false),uvco::ProcessStdio::createPipe(out,false,true)};
 auto p=uvco::spawn(loop,o); CHECK(p);
 CHECK(uvco::run(loop,exchange(*p,in,out))=="hello cat\n");
}
TEST_CASE("process kill and cached exit; repeat wait") {
 uvco::Loop loop; auto p=uvco::spawn(loop,sleeper()); CHECK(p && p->pid()>0);
 CHECK(p->kill(SIGTERM));
 uvco::drain(loop); // exit callback arrives before wait
 auto r=uvco::run(loop,waitFor(*p)); CHECK(r && r->termSignal==SIGTERM);
 auto again=uvco::run(loop,waitFor(*p)); CHECK(again && again->termSignal==SIGTERM);
 CHECK(not p->kill(SIGTERM));
}
TEST_CASE("process cross-thread cancel wait does not kill; can wait again") {
 uvco::Loop loop; auto p=uvco::spawn(loop,sleeper()); CHECK(p);
 co2::stop_source stop;
 std::thread t{[&] { std::this_thread::sleep_for(std::chrono::milliseconds{20}); stop.request_stop(); }};
 auto r=uvco::run(loop,waitFor(*p),stop.get_token()); t.join();
 CHECK(not r && r.error()==uvco::Errc::canceled);
 CHECK(p->kill(0)); CHECK(p->kill(SIGKILL));
 r=uvco::run(loop,waitFor(*p)); CHECK(r && r->termSignal==SIGKILL);
}
TEST_CASE("process pre-stopped wait") {
 uvco::Loop loop; auto p=uvco::spawn(loop,sleeper()); CHECK(p);
 co2::stop_source stop; stop.request_stop();
 auto r=uvco::run(loop,waitFor(*p),stop.get_token());
 CHECK(not r && r.error()==uvco::Errc::canceled);
 CHECK(p->kill(SIGKILL)); CHECK(uvco::run(loop,waitFor(*p)));
}
TEST_CASE("process explicit close aborts wait without killing") {
 uvco::Loop loop; auto p=uvco::spawn(loop,sleeper()); CHECK(p);
 auto pid=p->pid(); auto h=co2::spawn(loop,waitFor(*p));
 uv_run(loop.raw(),UV_RUN_NOWAIT);
 { auto closing=p->close(); }
 CHECK(::kill(pid,0)==0); CHECK(::kill(pid,SIGKILL)==0);
 uvco::drain(loop); CHECK(h.isReady());
 auto r=h.join(); CHECK(not r && r.error()==uvco::Errc::aborted);
 int status=0; CHECK(::waitpid(pid,&status,WNOHANG)==-1 && errno==ECHILD);
}
TEST_CASE("process loop destruction aborts wait but reaps child before returning") {
 auto loop=std::unique_ptr<uvco::Loop>{new uvco::Loop};
 auto o=sleeper();o.args={"sleep","0.03"};auto p=uvco::spawn(*loop,o);CHECK(p);auto pid=p->pid();
 auto h=co2::spawn(*loop,waitFor(*p));uv_run(loop->raw(),UV_RUN_NOWAIT);
 loop.reset();CHECK(h.isReady());auto r=h.join();
 CHECK(not r && r.error()==uvco::Errc::aborted);
 int status=0;CHECK(::waitpid(pid,&status,WNOHANG)==-1 && errno==ECHILD);
}
TEST_CASE("process destructor hands running child to reap-only block") {
 uvco::Loop loop; uv_pid_t pid=0;
 {auto o=sleeper();o.args={"sleep","0.03"};auto p=uvco::spawn(loop,o);CHECK(p);pid=p->pid();}
 CHECK(::kill(pid,0)==0);uvco::drain(loop);
 int status=0;CHECK(::waitpid(pid,&status,WNOHANG)==-1 && errno==ECHILD);
}
TEST_CASE("process errors and fd cleanup") {
 auto base=fds();
 {
  uvco::Loop loop;
  for(int i=0;i<20;++i) {
   uvco::Pipe out{loop}; uvco::ProcessOptions o; o.file="/no/such/uvco-program";
   o.stdio={uvco::ProcessStdio::ignore(),uvco::ProcessStdio::createPipe(out,false,true)};
   auto p=uvco::spawn(loop,o); CHECK(not p && p.error()==uvco::Errc::no_such_file);
  }
  uvco::ProcessOptions empty; CHECK(not uvco::spawn(loop,empty));
 }
 CHECK(fds()==base);
}
TEST_CASE("process env and cwd") {
 uvco::Loop loop; uvco::Pipe out{loop}; uvco::ProcessOptions o;
 o.file=UVCO_TEST_PROCESS_HELPER;o.args={"envcwd"};
 o.env={"UVCO_TEST=ok"}; o.cwd=UVCO_TEST_PROCESS_DIR;
 o.stdio={uvco::ProcessStdio::ignore(),uvco::ProcessStdio::createPipe(out,false,true)};
 auto p=uvco::spawn(loop,o); CHECK(p);
 CHECK(uvco::run(loop,output(*p,out))==std::string{"ok:"}+UVCO_TEST_PROCESS_DIR);
}
TEST_CASE("process true false and successful spawn fd cleanup") {
 auto base=fds();
 {
  uvco::Loop loop;
  for(int i=0;i<20;++i) {
   uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER;o.args={"exit",std::to_string(i%2)};
   auto p=uvco::spawn(loop,o); CHECK(p);
   auto r=uvco::run(loop,waitFor(*p)); CHECK(r && r->exitStatus==i%2);
  }
 }
 CHECK(fds()==base);
}
TEST_CASE("process invalid stdio rejected without libuv assertion") {
 uvco::Loop loop; auto o=sleeper(); uvco::Pipe p{loop};
 o.stdio={uvco::ProcessStdio::createPipe(p,false,false)};
 auto r=uvco::spawn(loop,o); CHECK(not r && r.error()==uvco::Errc::invalid_argument);
}
TEST_CASE("process inherit fd and detached option") {
 uvco::Loop loop; uvco::ProcessOptions o; o.file=UVCO_TEST_PROCESS_HELPER;o.args={"exit","0"}; o.detached=true;
 o.stdio={uvco::ProcessStdio::inherit(0),uvco::ProcessStdio::inherit(1),uvco::ProcessStdio::inherit(2)};
 auto p=uvco::spawn(loop,o); CHECK(p); auto r=uvco::run(loop,waitFor(*p));
 CHECK(r && r->exitStatus==0);
 co2::stop_source stop; stop.request_stop();
 r=uvco::run(loop,waitFor(*p),stop.get_token()); CHECK(not r && r.error()==uvco::Errc::canceled);
}
auto delayedWait(uvco::detail::ProcessWaitAwaiter& op)
 CO2_BEG(co2::Task<uvco::Result<uvco::ExitStatus>>, (op), uvco::Result<uvco::ExitStatus> r;) {
 CO2_AWAIT_SET(r,std::move(op));CO2_RETURN(r);
} CO2_END
TEST_CASE("regression: unstarted wait survives close") {
 uvco::Loop loop;uvco::ProcessOptions o;o.file=UVCO_TEST_PROCESS_HELPER;o.args={"exit","0"};
 auto p=uvco::spawn(loop,o);CHECK(p);auto op=p->wait();
 {auto c=p->close();}uvco::drain(loop);
 auto r=uvco::run(loop,delayedWait(op));CHECK(not r && r.error()==uvco::Errc::aborted);
}
TEST_CASE("process detach transfers responsibility explicitly") {
 uvco::Loop loop;auto p=uvco::spawn(loop,sleeper());CHECK(p);
 auto pid=p->detach();CHECK(not p->isOpen());uvco::drain(loop);
 CHECK(::kill(pid,0)==0);CHECK(::kill(pid,SIGKILL)==0);
 int status=0;CHECK(::waitpid(pid,&status,0)==pid);
}
UVCO_TEST_MAIN
