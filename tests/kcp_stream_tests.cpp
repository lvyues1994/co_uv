#include <cstdio>
#if __has_include(<uvco/kcp/stream.hpp>)
#include <uvco/kcp/stream.hpp>
#include <uvco/uvco.hpp>
#include "test.hpp"
using namespace uvco;
auto writer(KcpStream &s) CO2_BEG(co2::Task<>,(s),std::string data;std::size_t offset=0;Result<std::size_t> r{Errc::canceled};) {
    data.assign(100000, 's');
    while (offset < data.size()) {
        CO2_AWAIT_SET(r, s.write({data.data() + offset, data.size() - offset}));
        CHECK(r && *r > 0);
        offset += *r;
    }
    CO2_RETURN();
}
CO2_END
auto reader(KcpStream &s) CO2_BEG(co2::Task<>,(s),char bytes[7]{};std::size_t total=0;Result<std::size_t> r{Errc::canceled};) {
    CO2_AWAIT_SET(r, s.readSome({nullptr, 0}));
    CHECK(r && *r == 0);
    while (total < 100000) {
        CO2_AWAIT_SET(r, s.readSome(buffer(bytes)));
        CHECK(r && *r > 0 && *r <= 7);
        for (std::size_t i = 0; i < *r; ++i)
            CHECK(bytes[i] == 's');
        total += *r;
    }
    CHECK(total == 100000);
    CO2_RETURN();
}
CO2_END
auto both(KcpStream &a, KcpStream &b) CO2_BEG(co2::Task<>,(a,b)) {
    CO2_AWAIT(co2::whenAll(writer(a), reader(b)));
    CO2_RETURN();
}
CO2_END
template <class H> auto closeH(H &h) CO2_BEG(co2::Task<>,(h)) {
    CO2_AWAIT(h.close());
    CO2_RETURN();
}
CO2_END
TEST_CASE("KcpStream bounded leftover and large short writes preserve every byte") {
    Loop loop;
    auto address = *SocketAddress::ipv4("127.0.0.1", 0);
    auto a = KcpEndpoint::create(loop, address), b = KcpEndpoint::create(loop, address);
    CHECK(a && b);
    auto sa = a->openStream(*b->localAddress(), 8), sb = b->openStream(*a->localAddress(), 8);
    CHECK(sa && sb);
    run(loop, both(*sa, *sb));
    run(loop, closeH(*sa));
    run(loop, closeH(*sb));
    run(loop, closeH(*a));
    run(loop, closeH(*b));
    drain(loop);
}
UVCO_TEST_MAIN
#else
int main() {
    std::fputs("FAIL: separate KcpStream API missing\n", stderr);
    return 1;
}
#endif
