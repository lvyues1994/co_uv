#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
auto blocked(uvco::Loop& loop,co2::stop_source& stop,bool deadline) CO2_BEG(co2::Task<>,(loop,stop,deadline),uvco::Tcp server{loop};uvco::Tcp client{loop};uvco::Tcp peer;uvco::Result<uvco::Tcp> accepted;uvco::Result<void> r;std::string bytes;int size=4096;uv_timer_t* timer=nullptr;) {
 CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());
 CO2_AWAIT_SET(r,client.connect(*server.localAddress()));CHECK(r);
 CO2_AWAIT_SET(accepted,server.accept());CHECK(accepted);peer=std::move(*accepted);
 CHECK(uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(client.raw()),&size)==0);
 bytes.assign(8U<<20,'x');
 if(!deadline){
 timer=new uv_timer_t{};CHECK(uv_timer_init(loop.raw(),timer)==0);timer->data=&stop;
 CHECK(uv_timer_start(timer,[](uv_timer_t* t){static_cast<co2::stop_source*>(t->data)->request_stop();uv_close(reinterpret_cast<uv_handle_t*>(t),[](uv_handle_t* h){delete reinterpret_cast<uv_timer_t*>(h);});},20,0)==0);
 }
 if(deadline){
 CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::milliseconds{20},client.write(uvco::buffer(bytes))));
 }else{
 CO2_AWAIT_SET(r,client.write(uvco::buffer(bytes)));
 }
 CHECK(!r);CHECK(r.error()==(deadline?uvco::Errc::timed_out:uvco::Errc::canceled));
 CHECK(uv_is_closing(reinterpret_cast<uv_handle_t*>(client.raw()))!=0);
 if(deadline){
 CO2_AWAIT_SET(r,client.write(uvco::buffer("x",1)));CHECK(!r);CHECK(r.error()==uvco::Errc::bad_descriptor);
 }
} CO2_END
}
TEST_CASE("timeout on a blocked write closes stream and drains the write callback") {uvco::Loop loop;co2::stop_source stop;uvco::run(loop,blocked(loop,stop,true));}
TEST_CASE("direct pending write stop returns canceled after closing the stream") {uvco::Loop loop;co2::stop_source stop;uvco::run(loop,blocked(loop,stop,false),stop.get_token());}
UVCO_TEST_MAIN
