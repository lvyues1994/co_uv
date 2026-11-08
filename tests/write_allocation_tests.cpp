#include "allocation_counter.hpp"
#include "test.hpp"
#include <uvco/uvco.hpp>
extern "C" int __wrap_uv_try_write(uv_stream_t *, uv_buf_t const bufs[], unsigned n) {
    std::size_t total = 0;
    for (unsigned i = 0; i < n; ++i)
        total += bufs[i].len;
    return static_cast<int>(total);
}
namespace {
auto hotWrite(uvco::Tcp &tcp) CO2_BEG(co2::Task<>,(tcp),uvco::Result<void> r;std::size_t count=0;) {
    allocation_test::begin();
    CO2_AWAIT_SET(r, tcp.write(uvco::buffer("hello", 5)));
    count = allocation_test::end();
    CHECK(r);
    CHECK(count == 0);
}
CO2_END
}
TEST_CASE("right-value hot write await fits co2 slot and never allocates") {
    uvco::Loop loop;
    uvco::Tcp tcp{loop};
    uvco::run(loop, hotWrite(tcp));
    CHECK(sizeof(decltype(tcp.write(uvco::ConstBuffer{}))) <= CO2_AWAIT_STORAGE_SIZE);
}
UVCO_TEST_MAIN
