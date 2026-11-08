#include <cstdio>
#if __has_include("../benchmarks/latency_stats.hpp")
#include "../benchmarks/latency_stats.hpp"
#include "test.hpp"
TEST_CASE("Latency percentiles use nearest rank and retain max") {
    std::vector<double> values;
    for (int i = 100; i >= 1; --i)
        values.push_back(i);
    auto s = uvco_bench::summarize(values);
    CHECK(s.p50 == 50 && s.p99 == 99 && s.max == 100);
    s = uvco_bench::summarize({7});
    CHECK(s.p50 == 7 && s.p99 == 7 && s.max == 7);
}
UVCO_TEST_MAIN
#else
int main() {
    std::fputs("FAIL: latency summary missing\n", stderr);
    return 1;
}
#endif
