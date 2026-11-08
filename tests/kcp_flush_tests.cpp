#include <cstdio>
#if __has_include(<uvco/kcp/detail/driver_stats.hpp>)
#include <uvco/kcp/runtime.hpp>
#include <uvco/uvco.hpp>
#include "test.hpp"
using namespace uvco;
auto sends(KcpSession& s) CO2_BEG(co2::Task<>,(s),Result<void> r;) {
    CO2_AWAIT_SET(r,s.sendMessage(KcpMessage{'a'}));CHECK(r);
    CO2_AWAIT_SET(r,s.sendMessage(KcpMessage{'b'}));CHECK(r);CO2_RETURN();
} CO2_END
template<class H>auto closeH(H& h) CO2_BEG(co2::Task<>,(h)) {CO2_AWAIT(h.close());CO2_RETURN();} CO2_END
TEST_CASE("Input ACK admission and immediate flush share one explicit flush per driver batch") {
    Loop loop;auto addr=*SocketAddress::ipv4("127.0.0.1",0);
    auto a=KcpEndpoint::create(loop,addr),b=KcpEndpoint::create(loop,addr);CHECK(a&&b);
    KcpSessionOptions o;o.highWater=1;o.lowWater=0;
    auto sa=a->openSession(*b->localAddress(),1,o),sb=b->openSession(*a->localAddress(),1);CHECK(sa&&sb);
    run(loop,sends(*sa));auto stats=sa->stats();CHECK(stats.admissions==2&&stats.inputBatches>0);
    CHECK(stats.maxExplicitFlushesPerDrive<=1);
    run(loop,closeH(*sa));run(loop,closeH(*sb));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}
UVCO_TEST_MAIN
#else
int main(){std::fputs("FAIL: explicit-flush driver diagnostics missing\n",stderr);return 1;}
#endif
