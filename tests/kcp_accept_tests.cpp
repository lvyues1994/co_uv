#include <cstdio>
#if __has_include(<uvco/kcp/endpoint.hpp>)
#include <uvco/kcp/endpoint.hpp>
#include <uvco/uvco.hpp>
#include "test.hpp"
using namespace uvco;
auto sender(KcpSession& s) CO2_BEG(co2::Task<>,(s),Result<void> r;) {
    CO2_AWAIT_SET(r,s.sendMessage(KcpMessage{'h','i'}));CHECK(r);CO2_RETURN();
} CO2_END
auto accept(KcpEndpoint& e) CO2_BEG(co2::Task<>,(e),Result<KcpIncoming> candidate{Errc::canceled};Result<KcpSession> session{Errc::canceled};Result<KcpMessage> data{Errc::canceled};) {
    CO2_AWAIT_SET(candidate,e.acceptPeer());CHECK(candidate&&candidate->conv==73);
    CHECK(e.sessionCount()==0);
    session=e.openSession(std::move(*candidate));CHECK(session);
    CO2_AWAIT_SET(data,session->recvMessage());CHECK(data&&*data==KcpMessage({'h','i'}));
    CO2_AWAIT(session->close());CO2_RETURN();
} CO2_END
auto both(KcpSession& s,KcpEndpoint& e) CO2_BEG(co2::Task<>,(s,e)) {
    CO2_AWAIT(co2::whenAll(sender(s),accept(e)));CO2_RETURN();
} CO2_END
template<class H>auto closeH(H& h) CO2_BEG(co2::Task<>,(h)) {CO2_AWAIT(h.close());CO2_RETURN();} CO2_END
TEST_CASE("Unknown conv creates a bounded proposal, never a session; explicit accept replays first packet") {
    Loop loop;auto local=*SocketAddress::ipv4("127.0.0.1",0);KcpEndpointOptions o;o.accept=true;
    auto client=KcpEndpoint::create(loop,local),server=KcpEndpoint::create(loop,local,o);CHECK(client&&server);
    auto s=client->openSession(*server->localAddress(),73);CHECK(s);
    run(loop,both(*s,*server));CHECK(server->sessionCount()==0);
    run(loop,closeH(*s));run(loop,closeH(*client));run(loop,closeH(*server));drain(loop);
}
auto flood(Udp& udp,SocketAddress to) CO2_BEG(co2::Task<>,(udp,to),std::vector<char> packet;int n=0;Result<void> r;) {
    packet.assign(25,0);packet[4]=81;packet[6]=127;packet[20]=1;packet[24]='x';
    for(n=1;n<=50;++n){packet[0]=static_cast<char>(n);
        CO2_AWAIT_SET(r,udp.send(to,buffer(packet)));CHECK(r);
    }CO2_RETURN();
} CO2_END
auto settle(Loop& loop,KcpEndpoint& e) CO2_BEG(co2::Task<>,(loop,e),int n=0;Result<void> r;) {
    while(e.candidateCount()<4&&n++<100){
        CO2_AWAIT_SET(r,sleep(loop,std::chrono::milliseconds{5}));CHECK(r);
    }CO2_RETURN();
} CO2_END
TEST_CASE("Unknown conv flood is bounded globally and per peer") {
    Loop loop;auto local=*SocketAddress::ipv4("127.0.0.1",0);KcpEndpointOptions o;o.accept=true;
    auto e=KcpEndpoint::create(loop,local,o);auto u=Udp::create(loop);CHECK(e&&u);CHECK(u->bind(local));
    run(loop,flood(*u,*e->localAddress()));run(loop,settle(loop,*e));
    CHECK(e->sessionCount()==0&&e->candidateCount()==4);
    run(loop,closeH(*u));run(loop,closeH(*e));drain(loop);
}
UVCO_TEST_MAIN
#else
int main(){std::fputs("FAIL: bounded acceptPeer API missing\n",stderr);return 1;}
#endif
