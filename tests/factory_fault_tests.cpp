#include <new>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {int mode=0;bool failAllocation=false;}
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t n){if(failAllocation){failAllocation=false;throw std::bad_alloc{};}return __real__Znwm(n);}
extern "C" int __real_uv_tcp_init(uv_loop_t*,uv_tcp_t*);
extern "C" int __wrap_uv_tcp_init(uv_loop_t* l,uv_tcp_t* h){return mode==1?UV_EMFILE:__real_uv_tcp_init(l,h);}
extern "C" int __real_uv_udp_init(uv_loop_t*,uv_udp_t*);
extern "C" int __wrap_uv_udp_init(uv_loop_t* l,uv_udp_t* h){return mode==2?UV_EMFILE:__real_uv_udp_init(l,h);}
extern "C" int __real_uv_timer_init(uv_loop_t*,uv_timer_t*);
extern "C" int __wrap_uv_timer_init(uv_loop_t* l,uv_timer_t* h){return mode==3?UV_EMFILE:__real_uv_timer_init(l,h);}
extern "C" int __real_uv_async_init(uv_loop_t*,uv_async_t*,uv_async_cb);
extern "C" int __wrap_uv_async_init(uv_loop_t* l,uv_async_t* h,uv_async_cb cb){return mode==4?UV_EMFILE:__real_uv_async_init(l,h,cb);}
TEST_CASE("Result factories preserve native init errors and map allocation failure") {
 uvco::Loop loop;
 mode=1;CHECK(uvco::Tcp::create(loop).error()==static_cast<uvco::Errc>(UV_EMFILE));
 mode=2;CHECK(uvco::Udp::create(loop).error()==static_cast<uvco::Errc>(UV_EMFILE));
 mode=3;CHECK(uvco::Timer::create(loop).error()==static_cast<uvco::Errc>(UV_EMFILE));
 mode=4;CHECK(uvco::DnsResolver::create(loop).error()==static_cast<uvco::Errc>(UV_EMFILE));mode=0;
 failAllocation=true;CHECK(uvco::Tcp::create(loop).error()==uvco::Errc::no_memory);
 failAllocation=true;CHECK(uvco::Udp::create(loop).error()==uvco::Errc::no_memory);
 failAllocation=true;CHECK(uvco::Timer::create(loop).error()==uvco::Errc::no_memory);
 failAllocation=true;CHECK(uvco::DnsResolver::create(loop).error()==uvco::Errc::no_memory);
 CHECK(uvco::Tcp::create(loop));CHECK(uvco::Udp::create(loop));CHECK(uvco::Timer::create(loop));CHECK(uvco::DnsResolver::create(loop));
}
UVCO_TEST_MAIN
