#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
auto blocked(uvco::Loop& loop) CO2_BEG(co2::Task<>,(loop),uvco::Tcp server{loop};uvco::Tcp client{loop};uvco::Tcp peer;uvco::Result<uvco::Tcp> accepted;uvco::Result<void> r;std::string bytes;int size=4096;) {
 CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());
 CO2_AWAIT_SET(r,client.connect(*server.localAddress()));CHECK(r);
 CO2_AWAIT_SET(accepted,server.accept());CHECK(accepted);peer=std::move(*accepted);
 CHECK(uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(client.raw()),&size)==0);
 bytes.assign(8U<<20,'x');
 CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::milliseconds{20},client.write(uvco::buffer(bytes))));
 CHECK(!r);CHECK(r.error()==uvco::Errc::timed_out);
 CO2_AWAIT_SET(r,client.write(uvco::buffer("x",1)));CHECK(!r);CHECK(r.error()==uvco::Errc::bad_descriptor);
} CO2_END
}
TEST_CASE("timeout on a blocked write closes stream and drains the write callback") {uvco::Loop loop;uvco::run(loop,blocked(loop));}
UVCO_TEST_MAIN
