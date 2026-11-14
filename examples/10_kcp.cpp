#include <memory>
#include <co2/co2.hpp>
#include <uvco/kcp.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

auto sessionPing(uvco::KcpSession& a, uvco::KcpSession& b)
    CO2_BEG(co2::Task<int>, (a, b), uvco::Result<void> w; uvco::Result<uvco::KcpMessage> m;
            uvco::KcpMessage payload;) {
    payload = {'p', 'i', 'n', 'g'};
    CO2_AWAIT_SET(w, a.sendMessage(payload));
    if (not expect(w, "sendMessage")) CO2_RETURN(1);
    CO2_AWAIT_SET(m, b.recvMessage());
    if (not expect(m, "recvMessage") || *m != payload) CO2_RETURN(fail("kcp payload"));
    CO2_AWAIT_SET(w, a.drain());
    if (not expect(w, "drain")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

auto closeSession(uvco::KcpSession& s) CO2_BEG(co2::Task<>, (s)) {
    CO2_AWAIT(s.close());
    CO2_RETURN();
}
CO2_END

auto closeEndpoint(uvco::KcpEndpoint& e) CO2_BEG(co2::Task<>, (e)) {
    CO2_AWAIT(e.close());
    CO2_RETURN();
}
CO2_END

auto closeStream(uvco::KcpStream& s) CO2_BEG(co2::Task<>, (s)) {
    CO2_AWAIT(s.close());
    CO2_RETURN();
}
CO2_END

static int adoptSession(uvco::Result<uvco::KcpSession> result, char const* what,
                        std::unique_ptr<uvco::KcpSession>& out) {
    if (not expect(result, what)) return 1;
    out.reset(new uvco::KcpSession(std::move(*result)));
    return 0;
}

static int adoptStream(uvco::Result<uvco::KcpStream> result, char const* what,
                       std::unique_ptr<uvco::KcpStream>& out) {
    if (not expect(result, what)) return 1;
    out.reset(new uvco::KcpStream(std::move(*result)));
    return 0;
}

auto acceptAndStream(uvco::KcpEndpoint& server, uvco::KcpEndpoint& clientEp,
                     uvco::KcpSession& client)
    CO2_BEG(co2::Task<int>, (server, clientEp, client),
            uvco::Result<uvco::SocketAddress> addr; uvco::Result<uvco::SocketAddress> clientAddr;
            uvco::Result<uvco::KcpIncoming> incoming; uvco::Result<void> w;
            uvco::Result<uvco::KcpMessage> m; uvco::Result<std::size_t> n;
            uvco::KcpMessage payload; char buf[8]{};
            std::unique_ptr<uvco::KcpSession> accepted;
            std::unique_ptr<uvco::KcpStream> clientStream;
            std::unique_ptr<uvco::KcpStream> serverStream;) {
    payload = {'p', 'o', 'n', 'g'};
    CO2_AWAIT_SET(w, client.sendMessage(payload));
    if (not expect(w, "client send")) CO2_RETURN(1);
    CO2_AWAIT_SET(incoming, server.acceptPeer());
    if (not expect(incoming, "acceptPeer")) CO2_RETURN(1);
    if (adoptSession(server.openSession(std::move(*incoming)), "open accepted", accepted) != 0)
        CO2_RETURN(1);
    CO2_AWAIT_SET(m, accepted->recvMessage());
    if (not expect(m, "accepted recv") || *m != payload) CO2_RETURN(fail("accept payload"));

    CO2_AWAIT(client.close());
    CO2_AWAIT(accepted->close());

    clientAddr = clientEp.localAddress();
    addr = server.localAddress();
    if (not expect(clientAddr, "client local") || not expect(addr, "server local")) CO2_RETURN(1);
    if (adoptStream(clientEp.openStream(*addr, 9), "client stream", clientStream) != 0)
        CO2_RETURN(1);
    if (adoptStream(server.openStream(*clientAddr, 9), "server stream", serverStream) != 0)
        CO2_RETURN(1);
    CO2_AWAIT_SET(n, clientStream->write(uvco::buffer("hi", 2)));
    if (not expect(n, "stream write") || *n != 2) CO2_RETURN(fail("stream write size"));
    CO2_AWAIT_SET(n, serverStream->readSome(uvco::buffer(buf)));
    if (not expect(n, "stream read") || *n != 2 || buf[0] != 'h') CO2_RETURN(fail("stream payload"));

    CO2_AWAIT(clientStream->close());
    CO2_AWAIT(serverStream->close());
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    auto bind = uvco::SocketAddress::parse("127.0.0.1", 0);
    if (not expect(bind, "parse")) return 1;

    auto left = uvco::KcpEndpoint::create(loop, *bind);
    auto right = uvco::KcpEndpoint::create(loop, *bind);
    if (not expect(left, "left endpoint") || not expect(right, "right endpoint")) return 1;
    auto laddr = left->localAddress();
    auto raddr = right->localAddress();
    if (not expect(laddr, "left local") || not expect(raddr, "right local")) return 1;
    auto a = left->openSession(*raddr, 1);
    auto b = right->openSession(*laddr, 1);
    if (not expect(a, "open left") || not expect(b, "open right")) return 1;

    int const sessionRc = uvco::run(loop, sessionPing(*a, *b));
    uvco::run(loop, closeSession(*a));
    uvco::run(loop, closeSession(*b));
    uvco::run(loop, closeEndpoint(*left));
    uvco::run(loop, closeEndpoint(*right));
    if (sessionRc != 0) return sessionRc;

    uvco::KcpEndpointOptions opts;
    opts.accept = true;
    auto server = uvco::KcpEndpoint::create(loop, *bind, opts);
    auto clientEp = uvco::KcpEndpoint::create(loop, *bind);
    if (not expect(server, "accept server") || not expect(clientEp, "accept client")) return 1;
    auto saddr = server->localAddress();
    if (not expect(saddr, "server local")) return 1;
    auto client = clientEp->openSession(*saddr, 7);
    if (not expect(client, "open client session")) return 1;

    int const acceptRc = uvco::run(loop, acceptAndStream(*server, *clientEp, *client));
    uvco::run(loop, closeEndpoint(*clientEp));
    uvco::run(loop, closeEndpoint(*server));
    uvco::drain(loop);
    return acceptRc;
}
