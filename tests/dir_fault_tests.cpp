#include <new>
#include <stdexcept>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {int fault=0,armFault=0,closes=0;co2::stop_source* stop=nullptr;uv_fs_cb next=nullptr;}
extern "C" void* UVCO_TEST_REAL_NEW(std::size_t);
extern "C" void* UVCO_TEST_WRAP_NEW(std::size_t n){int mode=fault;fault=0;if(mode==1)throw std::bad_alloc{};if(mode==2)throw std::runtime_error{"projection"};return UVCO_TEST_REAL_NEW(n);}
extern "C" int __real_uv_fs_opendir(uv_loop_t*,uv_fs_t*,char const*,uv_fs_cb);
extern "C" int __real_uv_fs_closedir(uv_loop_t*,uv_fs_t*,uv_dir_t*,uv_fs_cb);
namespace {void callback(uv_fs_t* r){if(stop)stop->request_stop();fault=armFault;next(r);fault=0;}}
extern "C" int __wrap_uv_fs_opendir(uv_loop_t* l,uv_fs_t* r,char const* path,uv_fs_cb cb){next=cb;return __real_uv_fs_opendir(l,r,path,&callback);}
extern "C" int __wrap_uv_fs_closedir(uv_loop_t* l,uv_fs_t* r,uv_dir_t* dir,uv_fs_cb cb){++closes;return __real_uv_fs_closedir(l,r,dir,cb);}
namespace {auto request(uvco::Loop& loop) CO2_BEG(co2::Task<uvco::Result<uvco::fs::Dir>>,(loop),uvco::Result<uvco::fs::Dir> r;) {
 CO2_AWAIT_SET(r,uvco::fs::openDir(loop,UVCO_TEST_DIR_PATH));CO2_RETURN(std::move(r));
} CO2_END}
TEST_CASE("opendir late cancellation releases native directory before reporting canceled"){
 uvco::Loop loop;co2::stop_source source;stop=&source;int before=closes;auto r=uvco::run(loop,request(loop),source.get_token());stop=nullptr;
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(closes==before+1);
}
TEST_CASE("opendir bad_alloc releases native directory"){
 uvco::Loop loop;armFault=1;int before=closes;auto r=uvco::run(loop,request(loop));armFault=0;CHECK(!r);CHECK(r.error()==static_cast<uvco::Errc>(UV_ENOMEM));CHECK(closes==before+1);
}
TEST_CASE("opendir throwing construction releases native directory before rethrow"){
 uvco::Loop loop;armFault=2;int before=closes;bool caught=false;try{static_cast<void>(uvco::run(loop,request(loop)));}catch(std::runtime_error const&){caught=true;}
 armFault=0;CHECK(caught);CHECK(closes==before+1);
}
UVCO_TEST_MAIN
