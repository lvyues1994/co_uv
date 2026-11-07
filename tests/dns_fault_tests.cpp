#include <co2/co2.hpp>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {int fault=0;int frees=0;co2::stop_source* stop=nullptr;uv_getaddrinfo_cb next=nullptr;}
void* operator new(std::size_t size) {
 int const mode=fault;fault=0;
 if(mode==1)throw std::bad_alloc{};
 if(mode==2)throw std::runtime_error{"projection"};
 if(void* p=std::malloc(size?size:1))return p;
 throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
extern "C" int __real_uv_getaddrinfo(uv_loop_t*,uv_getaddrinfo_t*,uv_getaddrinfo_cb,char const*,char const*,addrinfo const*);
extern "C" void __real_uv_freeaddrinfo(addrinfo*);
namespace {
int armFault=0;
void callback(uv_getaddrinfo_t* req,int status,addrinfo* values) {
 if(stop)stop->request_stop();
 fault=armFault;next(req,status,values);fault=0;
}
}
extern "C" int __wrap_uv_getaddrinfo(uv_loop_t* loop,uv_getaddrinfo_t* req,uv_getaddrinfo_cb cb,char const* host,char const* service,addrinfo const* hints) {
 next=cb;return __real_uv_getaddrinfo(loop,req,&callback,host,service,hints);
}
extern "C" void __wrap_uv_freeaddrinfo(addrinfo* p) {++frees;__real_uv_freeaddrinfo(p);}
namespace {
auto request(uvco::Loop& loop) CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>,(loop),
 uvco::Result<std::vector<uvco::SocketAddress>> r;addrinfo hints{};) {
 hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;hints.ai_socktype=SOCK_STREAM;
 CO2_AWAIT_SET(r,uvco::getaddrinfo(loop,"127.0.0.1","80",hints));CO2_RETURN(std::move(r));
} CO2_END
}
TEST_CASE("DNS frees successful resolver data when cancellation wins at callback entry") {
 uvco::Loop loop;co2::stop_source source;stop=&source;int const before=frees;
 auto r=uvco::run(loop,request(loop),source.get_token());stop=nullptr;
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(frees==before+1);
}
TEST_CASE("DNS projection bad_alloc maps to ENOMEM and frees native data") {
 uvco::Loop loop;armFault=1;int const before=frees;auto r=uvco::run(loop,request(loop));armFault=0;
 CHECK(!r);CHECK(r.error()==static_cast<uvco::Errc>(UV_ENOMEM));CHECK(frees==before+1);
}
TEST_CASE("DNS projection other exception frees native data before rethrow") {
 uvco::Loop loop;armFault=2;int const before=frees;bool caught=false;
 try{static_cast<void>(uvco::run(loop,request(loop)));}catch(std::runtime_error const& e){caught=std::string{e.what()}=="projection";}
 armFault=0;CHECK(caught);CHECK(frees==before+1);
}
UVCO_TEST_MAIN
