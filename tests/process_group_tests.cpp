#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/process/process.hpp"
#include "uvco/core/run.hpp"
namespace {
auto wait(uvco::Process& p) CO2_BEG(co2::Task<uvco::Result<uvco::ExitStatus>>,(p),uvco::Result<uvco::ExitStatus> r;) {
 CO2_AWAIT_SET(r,p.wait());CO2_RETURN(r);
} CO2_END
#ifndef _WIN32
auto group(uvco::Process& p,uvco::Pipe& output)
 CO2_BEG(co2::Task<>,(p,output),uvco::Result<std::size_t> n;uvco::Result<uvco::ExitStatus> exit;char bytes[64];std::string text;) {
 while(text.find("ready\n")==std::string::npos){CO2_AWAIT_SET(n,output.read(uvco::buffer(bytes)));CHECK(n);text.append(bytes,*n);}
 CHECK(p.signalGroup(SIGTERM));
 CO2_AWAIT_SET(exit,p.wait());CHECK(exit && exit->exitStatus==0);
 while(true){CO2_AWAIT_SET(n,output.read(uvco::buffer(bytes)));if(!n)break;text.append(bytes,*n);}
 CHECK(text.find("reaped\n")!=std::string::npos);CHECK(p.signalGroup(0).error()==static_cast<uvco::Errc>(UV_ESRCH));
} CO2_END
#endif
}
TEST_CASE("ordinary process never exposes a foreign process-group capability") {
 uvco::Loop loop;uvco::ProcessOptions options;options.file=UVCO_TEST_PROCESS_HELPER;options.args={"exit","0"};
 auto child=uvco::spawn(loop,options);CHECK(child);CHECK(child->signalGroup(0).error()==static_cast<uvco::Errc>(UV_ENOTSUP));
 CHECK(uvco::run(loop,wait(*child)));
}
#ifndef _WIN32
TEST_CASE("private process-group signal reaches grandchild and helper reaps it") {
 uvco::Loop loop;uvco::Pipe output{loop};uvco::ProcessOptions options;
 options.file=UVCO_TEST_PROCESS_GROUP_HELPER;options.privateGroup=true;
 options.stdio={uvco::ProcessStdio::ignore(),uvco::ProcessStdio::createPipe(output,false,true)};
 auto child=uvco::spawn(loop,options);CHECK(child);uvco::run(loop,group(*child,output));
}
#endif
UVCO_TEST_MAIN
