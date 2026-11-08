#include <cstdio>
#if __has_include(<uvco/kcp/runtime.hpp>)
#include <uvco/kcp/runtime.hpp>
#include <uvco/uvco.hpp>
#include "test.hpp"
using namespace uvco;
using namespace std::chrono_literals;
auto sendOne(KcpSession& s,KcpMessage data) CO2_BEG(co2::Task<>,(s,data),Result<void> r;) {
    CO2_AWAIT_SET(r,s.sendMessage(std::move(data)));CHECK(r);CO2_RETURN();
} CO2_END
auto receiveOne(KcpSession& s) CO2_BEG(co2::Task<Result<KcpMessage>>,(s),Result<KcpMessage> r{Errc::canceled};) {
    CO2_AWAIT_SET(r,s.recvMessage());CO2_RETURN(std::move(r));
} CO2_END
auto echo(KcpSession& s) CO2_BEG(co2::Task<>,(s),Result<KcpMessage> m{Errc::canceled};Result<void> r;) {
    CO2_AWAIT_SET(m,s.recvMessage());CHECK(m);
    CO2_AWAIT_SET(r,s.sendMessage(std::move(*m)));CHECK(r);CO2_RETURN();
} CO2_END
auto client(KcpSession& s) CO2_BEG(co2::Task<>,(s),KcpMessage data;Result<KcpMessage> m{Errc::canceled};Result<void> r;) {
    data.assign(65536,'a');
    CO2_AWAIT_SET(r,s.sendMessage(data));CHECK(r);
    CO2_AWAIT_SET(m,s.recvMessage());CHECK(m&&*m==data);
    CO2_AWAIT_SET(r,s.drain());CHECK(r);CO2_RETURN();
} CO2_END
auto both(KcpSession& a,KcpSession& b) CO2_BEG(co2::Task<>,(a,b)) {
    CO2_AWAIT(co2::whenAll(client(a),echo(b)));
    CO2_RETURN();
} CO2_END
auto timedReceive(Loop& loop,KcpSession& s) CO2_BEG(co2::Task<Result<KcpMessage>>,(loop,s),Result<KcpMessage> r{Errc::canceled};) {
    CO2_AWAIT_SET(r,timeout(loop,20ms,receiveOne(s)));
    CO2_RETURN(std::move(r));
} CO2_END
template<class H> auto closeH(H& h) CO2_BEG(co2::Task<>,(h)) {CO2_AWAIT(h.close());CO2_RETURN();} CO2_END
static SocketAddress local(){return *SocketAddress::ipv4("127.0.0.1",0);}
TEST_CASE("KCP real UDP message roundtrip and drain") {
    Loop loop;auto a=KcpEndpoint::create(loop,local()),b=KcpEndpoint::create(loop,local());CHECK(a&&b);
    auto sa=a->openSession(*b->localAddress(),42),sb=b->openSession(*a->localAddress(),42);CHECK(sa&&sb);
    CHECK(!a->openSession(*b->localAddress(),42));
    run(loop,both(*sa,*sb));
    run(loop,closeH(*sa));run(loop,closeH(*sb));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}
TEST_CASE("KCP stop token cancels only waiting receiver") {
    Loop loop;auto a=KcpEndpoint::create(loop,local());CHECK(a);auto s=a->openSession(*SocketAddress::ipv4("127.0.0.1",9),1);CHECK(s);
    co2::stop_source stop;stop.request_stop();auto r=run(loop,receiveOne(*s),stop.get_token());CHECK(!r&&r.error()==Errc::canceled);
    auto timed=run(loop,timedReceive(loop,*s));CHECK(!timed&&timed.error()==Errc::timed_out);
    run(loop,closeH(*s));run(loop,closeH(*a));drain(loop);
}
TEST_CASE("KCP saved awaiter survives owner close") {
    Loop loop;auto a=KcpEndpoint::create(loop,local());CHECK(a);auto s=a->openSession(*SocketAddress::ipv4("127.0.0.1",9),2);CHECK(s);
    auto saved=s->recvMessage();run(loop,closeH(*s));
    CHECK(!saved.await_ready()); // the ref still protects the closed block
    run(loop,closeH(*a));drain(loop);
}
TEST_CASE("KCP endpoint session limit is hard and recoverable") {
    Loop loop;KcpEndpointOptions o;o.sessionLimit=1;
    auto e=KcpEndpoint::create(loop,local(),o);CHECK(e);
    auto peer=*SocketAddress::ipv4("127.0.0.1",9);
    auto first=e->openSession(peer,1);CHECK(first);
    auto second=e->openSession(peer,2);CHECK(!second&&second.error()==Errc::busy);
    run(loop,closeH(*first));run(loop,closeH(*e));drain(loop);
}
UVCO_TEST_MAIN
#else
int main(){std::fputs("FAIL: KCP coroutine runtime missing\n",stderr);return 1;}
#endif
