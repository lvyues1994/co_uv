#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

auto echoOnce(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<uvco::SocketAddress> parsed; uvco::Result<void> r;
        uvco::Result<uvco::SocketAddress> addr; uvco::Result<uvco::Tcp> accepted;
        uvco::Result<std::size_t> n; char buf[16]{};
        uvco::Tcp server{loop}; uvco::Tcp client{loop};) {
    parsed = uvco::SocketAddress::parse("127.0.0.1", 0);
    if (not expect(parsed, "parse")) CO2_RETURN(1);
    r = server.bind(*parsed);
    if (not expect(r, "bind")) CO2_RETURN(1);
    r = server.listen();
    if (not expect(r, "listen")) CO2_RETURN(1);
    addr = server.localAddress();
    if (not expect(addr, "localAddress")) CO2_RETURN(1);

    CO2_AWAIT_SET(r, client.connect(*addr));
    if (not expect(r, "connect")) CO2_RETURN(1);
    CO2_AWAIT_SET(accepted, server.accept());
    if (not expect(accepted, "accept")) CO2_RETURN(1);

    CO2_AWAIT_SET(r, client.write(uvco::buffer("hello", 5)));
    if (not expect(r, "write")) CO2_RETURN(1);
    CO2_AWAIT_SET(n, accepted->read(uvco::buffer(buf)));
    if (not expect(n, "read") || *n != 5 || buf[0] != 'h') CO2_RETURN(fail("echo mismatch"));

    CO2_AWAIT(client.close());
    CO2_AWAIT(accepted->close());
    CO2_AWAIT(server.close());
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, echoOnce(loop));
}
