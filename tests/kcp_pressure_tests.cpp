#include <cstdio>
#if __has_include(<uvco/kcp/detail/pressure.hpp>)
#include <uvco/kcp/detail/pressure.hpp>
#include "test.hpp"
TEST_CASE("Output congestion requires both sustained full duration and drop threshold") {
    uvco::detail::KcpOutputPressure p;
    CHECK(!p.observe(0, 1, false, 2000, 1024));
    CHECK(!p.observe(1999, 1024, false, 2000, 1024));
    CHECK(p.observe(2000, 1024, false, 2000, 1024));
    CHECK(!p.observe(2001, 1024, true, 2000, 1024));
    CHECK(!p.observe(10000, 1025, false, 2000, 1024));
    CHECK(!p.observe(20000, 1025, false, 2000, 1024));
    CHECK(p.observe(20001, 2048, false, 2000, 1024));
}
UVCO_TEST_MAIN
#else
int main() {
    std::fputs("FAIL: sustained KCP congestion guard missing\n", stderr);
    return 1;
}
#endif
