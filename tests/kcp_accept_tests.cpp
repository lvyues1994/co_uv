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

using namespace std::chrono_literals;
auto validateCandidate(Loop& loop,KcpEndpoint& e,KcpEndpoint& other,bool expire)
 CO2_BEG(co2::Task<>,(loop,e,other,expire),Result<KcpIncoming> c{Errc::canceled};Result<KcpSession> s{Errc::canceled};Result<void> delay;) {
    CO2_AWAIT_SET(c,e.acceptPeer());CHECK(c);
    CHECK(!other.openSession(std::move(*c)));
    if(expire){
        CO2_AWAIT_SET(delay,sleep(loop,50ms));CHECK(delay);
        CHECK(!e.openSession(std::move(*c)));
    }else{
        s=e.openSession(std::move(*c));CHECK(s);CHECK(!e.openSession(std::move(*c)));
        CO2_AWAIT(s->close());
    }CO2_RETURN();
} CO2_END
auto validateBoth(Loop& loop,KcpSession& s,KcpEndpoint& e,KcpEndpoint& other,bool expire)
 CO2_BEG(co2::Task<>,(loop,s,e,other,expire)) {
    CO2_AWAIT(co2::whenAll(sender(s),validateCandidate(loop,e,other,expire)));CO2_RETURN();
} CO2_END
TEST_CASE("Accept candidates are endpoint-bound single-use and expire") {
    for(bool expire:{false,true}){
        Loop loop;auto local=*SocketAddress::ipv4("127.0.0.1",0);KcpEndpointOptions o;o.accept=true;o.candidateTtlMs=20;
        auto a=KcpEndpoint::create(loop,local),b=KcpEndpoint::create(loop,local,o),other=KcpEndpoint::create(loop,local);CHECK(a&&b&&other);
        auto s=a->openSession(*b->localAddress(),73);CHECK(s);run(loop,validateBoth(loop,*s,*b,*other,expire));
        run(loop,closeH(*s));run(loop,closeH(*a));run(loop,closeH(*b));run(loop,closeH(*other));drain(loop);
    }
}
auto recordAccept(KcpEndpoint& e,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(e,errors),Result<KcpIncoming> c{Errc::canceled};) {
    CO2_AWAIT_SET(c,e.acceptPeer());CHECK(!c);errors.push_back(c.error());CO2_RETURN();
} CO2_END
auto acceptConflict(KcpEndpoint& e,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(e,errors)) {
    CO2_AWAIT(co2::whenAll(recordAccept(e,errors),recordAccept(e,errors)));CO2_RETURN();
} CO2_END
auto timedConflict(Loop& loop,KcpEndpoint& e,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(loop,e,errors)) {
    CO2_AWAIT(timeout(loop,10ms,acceptConflict(e,errors)));CO2_RETURN();
} CO2_END
auto closeLater(Loop& loop,KcpEndpoint& e) CO2_BEG(co2::Task<>,(loop,e),Result<void> r;) {
    CO2_AWAIT_SET(r,sleep(loop,10ms));CHECK(r);
    CO2_AWAIT(e.close());CO2_RETURN();
} CO2_END
auto closeAccept(Loop& loop,KcpEndpoint& e,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(loop,e,errors)) {
    CO2_AWAIT(co2::whenAll(recordAccept(e,errors),closeLater(loop,e)));CO2_RETURN();
} CO2_END
TEST_CASE("Accept wait handles busy cancellation and endpoint close") {
    Loop loop;auto local=*SocketAddress::ipv4("127.0.0.1",0);KcpEndpointOptions o;o.accept=true;
    auto e=KcpEndpoint::create(loop,local,o);CHECK(e);std::vector<Errc> errors;run(loop,timedConflict(loop,*e,errors));
    CHECK(errors.size()==2&&errors[0]==Errc::busy&&errors[1]==Errc::canceled);
    errors.clear();run(loop,closeAccept(loop,*e,errors));CHECK(errors.size()==1&&errors[0]==Errc::aborted);drain(loop);
}

auto settleGlobal(Loop& loop,KcpEndpoint& e) CO2_BEG(co2::Task<>,(loop,e),int n=0;Result<void> r;) {
    while(e.candidateCount()<3&&n++<100){
        CO2_AWAIT_SET(r,sleep(loop,5ms));CHECK(r);
    }CO2_RETURN();
} CO2_END
TEST_CASE("Unknown conv proposals obey the global cap independently of per-peer cap") {
    Loop loop;auto local=*SocketAddress::ipv4("127.0.0.1",0);KcpEndpointOptions o;o.accept=true;o.candidateLimit=3;o.perPeerCandidates=100;
    auto e=KcpEndpoint::create(loop,local,o);auto u=Udp::create(loop);CHECK(e&&u);CHECK(u->bind(local));
    run(loop,flood(*u,*e->localAddress()));run(loop,settleGlobal(loop,*e));CHECK(e->candidateCount()==3&&e->sessionCount()==0);
    run(loop,closeH(*u));run(loop,closeH(*e));drain(loop);
}
UVCO_TEST_MAIN
#else
int main(){std::fputs("FAIL: bounded acceptPeer API missing\n",stderr);return 1;}
#endif
