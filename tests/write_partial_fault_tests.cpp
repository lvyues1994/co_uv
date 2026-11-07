#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
extern "C" int __wrap_uv_try_write(uv_stream_t*,uv_buf_t const[],unsigned int){return 1;}
extern "C" int __wrap_uv_write(uv_write_t*,uv_stream_t*,uv_buf_t const[],unsigned int,uv_write_cb){return UV_EPIPE;}
namespace {
auto write(uvco::Tcp& tcp) CO2_BEG(co2::Task<>,(tcp),uvco::Result<void> r;) {
 CO2_AWAIT_SET(r,tcp.write(uvco::buffer("ab",2)));CHECK(!r);CHECK(r.error()==uvco::Errc::broken_pipe);
 CHECK(uv_is_closing(reinterpret_cast<uv_handle_t*>(tcp.raw()))!=0);
} CO2_END
}
TEST_CASE("partial try_write followed by submission failure closes ambiguous stream") {uvco::Loop loop;uvco::Tcp tcp{loop};uvco::run(loop,write(tcp));}
UVCO_TEST_MAIN
