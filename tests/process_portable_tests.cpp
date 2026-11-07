#include <string>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
auto result(uvco::Process& p)
 CO2_BEG(co2::Task<uvco::Result<uvco::ExitStatus>>, (p), uvco::Result<uvco::ExitStatus> r;) {
 CO2_AWAIT_SET(r,p.wait());CO2_RETURN(r);
} CO2_END
auto text(uvco::Process& p,uvco::Pipe& out)
 CO2_BEG(co2::Task<std::string>, (p,out),char buf[64]; std::string s;
 uvco::Result<std::size_t> n;uvco::Result<uvco::ExitStatus> status;) {
 for(;;){CO2_AWAIT_SET(n,out.read(uvco::buffer(buf)));if(not n)break;s.append(buf,*n);}
 CHECK(n.error()==uvco::Errc::eof);
 CO2_AWAIT_SET(status,p.wait());CHECK(status && status->exit_status==0);CO2_RETURN(s);
} CO2_END
}
TEST_CASE("portable process helper exit codes") {
 uvco::Loop loop;
 for(int code:{0,1,42}){
  uvco::ProcessOptions o;o.file=UVCO_TEST_PROCESS_HELPER;o.args={"exit",std::to_string(code)};
  auto p=uvco::spawn(loop,o);CHECK(p);auto r=uvco::run(loop,result(*p));
  CHECK(r && r->exit_status==code && r->term_signal==0);
 }
}
TEST_CASE("portable process helper stdout") {
 uvco::Loop loop;uvco::Pipe out{loop};uvco::ProcessOptions o;
 o.file=UVCO_TEST_PROCESS_HELPER;o.args={"echo","hello"};
 o.stdio={uvco::ProcessStdio::ignore(),uvco::ProcessStdio::createPipe(out,false,true)};
 auto p=uvco::spawn(loop,o);CHECK(p);CHECK(uvco::run(loop,text(*p,out))=="hello\n");
}
UVCO_TEST_MAIN
