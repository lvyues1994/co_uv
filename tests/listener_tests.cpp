#include <co2/co2.hpp>
#include <type_traits>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
template<class L,class S,class Address> auto connect(uvco::Loop& loop,L& listener,Address address)
 CO2_BEG(co2::Task<bool>,(loop,listener,address),S client{loop};uvco::Result<std::tuple<S>> accepted;) {
 CO2_AWAIT_SET(accepted,uvco::whenAllResults(listener.accept(),client.connect(address)));CO2_RETURN(static_cast<bool>(accepted));
} CO2_END
}
TEST_CASE("TcpListener facade accepts connected Tcp without exposing stream reads"){
 uvco::Loop loop;uvco::TcpListener listener{loop};auto a=uvco::SocketAddress::ipv4("127.0.0.1",0);CHECK(a);CHECK(listener.bind(*a));CHECK(listener.listen());
 auto bound=listener.localAddress();CHECK(bound);CHECK((uvco::run(loop,connect<uvco::TcpListener,uvco::Tcp>(loop,listener,*bound))));
 static_cast<void>(listener.close());loop.runUntilIdle();CHECK(!listener.isOpen());
}
#ifndef _WIN32
TEST_CASE("PipeListener facade accepts connected Pipe"){
 uvco::Loop loop;uvco::PipeListener listener{loop};std::string path=std::string{UVCO_TEST_PIPE_DIR}+"/listener-"+std::to_string(uv_os_getpid());
 CHECK(listener.bind(path));CHECK(listener.listen());CHECK((uvco::run(loop,connect<uvco::PipeListener,uvco::Pipe>(loop,listener,path))));
 static_cast<void>(listener.close());loop.runUntilIdle();uv_fs_t req{};uv_fs_unlink(nullptr,&req,path.c_str(),nullptr);uv_fs_req_cleanup(&req);
}
#endif
UVCO_TEST_MAIN
