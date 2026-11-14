#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

auto ping(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<uvco::SocketAddress> parsed; uvco::Result<void> r;
        uvco::Result<uvco::SocketAddress> addr; uvco::Result<uvco::Datagram> d;
        char buf[32]{}; uvco::Udp a{loop}; uvco::Udp b{loop};) {
    parsed = uvco::SocketAddress::parse("127.0.0.1", 0);
    if (not expect(parsed, "parse")) CO2_RETURN(1);
    r = a.bind(*parsed);
    if (not expect(r, "bind a")) CO2_RETURN(1);
    r = b.bind(*parsed);
    if (not expect(r, "bind b")) CO2_RETURN(1);
    addr = b.localAddress();
    if (not expect(addr, "localAddress")) CO2_RETURN(1);

    CO2_AWAIT_SET(r, a.send(*addr, uvco::buffer("ping", 4)));
    if (not expect(r, "send")) CO2_RETURN(1);
    CO2_AWAIT_SET(d, b.recv(uvco::buffer(buf)));
    if (not expect(d, "recv") || d->size != 4 || buf[0] != 'p') CO2_RETURN(fail("datagram mismatch"));
    if (d->truncated) CO2_RETURN(fail("unexpected truncation"));

    CO2_AWAIT(a.close());
    CO2_AWAIT(b.close());
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, ping(loop));
}
