#include <cstdio>
#if __has_include(<uvco/kcp/runtime.hpp>)
#include <uvco/kcp/runtime.hpp>
#ifdef __linux__
#include <dirent.h>
#endif
#include <uvco/uvco.hpp>
#include "test.hpp"
#ifdef UVCO_TEST_KCP_PENDING_SENDS
extern "C" int __wrap_uv_udp_try_send(uv_udp_t*,uv_buf_t const[],unsigned,sockaddr const*) {return UV_EAGAIN;}
#endif
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

auto waitSaved(detail::KcpReceiveAwaiter a) CO2_BEG(co2::Task<Result<KcpMessage>>,(a),Result<KcpMessage> r{Errc::canceled};) {
    CO2_AWAIT_SET(r,a);CO2_RETURN(std::move(r));
} CO2_END
auto closeLater(Loop& loop,KcpSession& s) CO2_BEG(co2::Task<>,(loop,s),Result<void> r;) {
    CO2_AWAIT_SET(r,sleep(loop,10ms));CHECK(r);
    CO2_AWAIT(s.close());CO2_RETURN();
} CO2_END
auto record(KcpSession& s,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(s,errors),Result<KcpMessage> r{Errc::canceled};) {
    CO2_AWAIT_SET(r,s.recvMessage());CHECK(!r);errors.push_back(r.error());CO2_RETURN();
} CO2_END
auto closePending(Loop& loop,KcpSession& s,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(loop,s,errors)) {
    CO2_AWAIT(co2::whenAll(record(s,errors),closeLater(loop,s)));CO2_RETURN();
} CO2_END
auto conflict(KcpSession& s,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(s,errors)) {
    CO2_AWAIT(co2::whenAll(record(s,errors),record(s,errors)));CO2_RETURN();
} CO2_END
auto timedConflict(Loop& loop,KcpSession& s,std::vector<Errc>& errors) CO2_BEG(co2::Task<>,(loop,s,errors)) {
    CO2_AWAIT(timeout(loop,20ms,conflict(s,errors)));CO2_RETURN();
} CO2_END
TEST_CASE("KCP pending close and concurrent receiver are recoverable") {
    Loop loop;auto e=KcpEndpoint::create(loop,local());CHECK(e);auto s=e->openSession(*SocketAddress::ipv4("127.0.0.1",9),1);CHECK(s);
    std::vector<Errc> errors;run(loop,timedConflict(loop,*s,errors));
    CHECK(errors.size()==2&&errors[0]==Errc::busy&&errors[1]==Errc::canceled);
    errors.clear();run(loop,closePending(loop,*s,errors));CHECK(errors.size()==1&&errors[0]==Errc::aborted);
    run(loop,closeH(*e));drain(loop);
}
TEST_CASE("KCP retained awaiter actually resumes after owner close") {
    Loop loop;auto e=KcpEndpoint::create(loop,local());CHECK(e);auto s=e->openSession(*SocketAddress::ipv4("127.0.0.1",9),1);CHECK(s);
    auto saved=s->recvMessage();run(loop,closeH(*s));auto r=run(loop,waitSaved(std::move(saved)));CHECK(!r&&r.error()==Errc::aborted);
    run(loop,closeH(*e));drain(loop);
}
TEST_CASE("KCP Loop-first destruction drains internal pumps and pending receiver") {
    std::unique_ptr<Loop> loop(new Loop);auto e=KcpEndpoint::create(*loop,local());CHECK(e);
    auto s=e->openSession(*SocketAddress::ipv4("127.0.0.1",9),1);CHECK(s);
    auto task=receiveOne(*s);detail::RunCompletion completion{*loop};
    detail::co2compat::TaskAccess::start(task,completion.handle(),{});
    loop.reset();CHECK(completion.finished);
    auto r=detail::co2compat::TaskAccess::takeResult(task);CHECK(!r&&r.error()==Errc::aborted);
}
auto oversizedReceiver(KcpSession& s) CO2_BEG(co2::Task<>,(s),Result<KcpMessage> r{Errc::canceled};Result<void> d;) {
    CO2_AWAIT_SET(r,s.recvMessage());CHECK(!r&&r.error()==static_cast<Errc>(UV_EMSGSIZE));
    CO2_AWAIT_SET(d,s.drain());CHECK(!d&&d.error()==static_cast<Errc>(UV_EMSGSIZE));CO2_RETURN();
} CO2_END
auto oversizedBoth(KcpSession& a,KcpSession& b) CO2_BEG(co2::Task<>,(a,b)) {
    CO2_AWAIT(co2::whenAll(sendOne(a,KcpMessage{'a','b'}),oversizedReceiver(b)));CO2_RETURN();
} CO2_END
TEST_CASE("KCP oversized receive terminally stops rather than wedging the queue head") {
    Loop loop;auto a=KcpEndpoint::create(loop,local()),b=KcpEndpoint::create(loop,local());CHECK(a&&b);
    KcpSessionOptions o;o.core.messageLimit=1;
    auto sa=a->openSession(*b->localAddress(),1),sb=b->openSession(*a->localAddress(),1,o);CHECK(sa&&sb);
    run(loop,oversizedBoth(*sa,*sb));run(loop,closeH(*sa));run(loop,closeH(*sb));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}

auto sendResult(KcpSession& s) CO2_BEG(co2::Task<Result<void>>,(s),Result<void> r;) {
    CO2_AWAIT_SET(r,s.sendMessage(KcpMessage{'a'}));CO2_RETURN(r);
} CO2_END
auto byteBudget(Loop& loop,KcpSession& s) CO2_BEG(co2::Task<>,(loop,s),Result<void> r;) {
    CO2_AWAIT_SET(r,s.sendMessage(KcpMessage{'a'}));CHECK(r);
    CO2_AWAIT_SET(r,timeout(loop,10ms,sendResult(s)));CHECK(!r&&r.error()==Errc::timed_out);
    CHECK(s.stats().waitingSegments==1);CO2_RETURN();
} CO2_END
TEST_CASE("KCP byte high water blocks independently of segment high water; cancel does not submit") {
    Loop loop;auto a=KcpEndpoint::create(loop,local()),b=KcpEndpoint::create(loop,local());CHECK(a&&b);
    KcpSessionOptions o;o.core.messageLimit=1;o.byteHighWater=1200;o.byteLowWater=0;o.byteHardLimit=2400;
    auto s=a->openSession(*b->localAddress(),1,o);CHECK(s);run(loop,byteBudget(loop,*s));
    run(loop,closeH(*s));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}

struct ExposedSession:KcpSession {
    explicit ExposedSession(KcpSession&& s):KcpSession{std::move(s)}{}
    detail::KcpSessionBlock& state(){return block();}
};
TEST_CASE("KCP completed receiver retains its lease until take") {
    Loop loop;auto a=KcpEndpoint::create(loop,local()),b=KcpEndpoint::create(loop,local());CHECK(a&&b);
    auto first=a->openSession(*b->localAddress(),1),peer=b->openSession(*a->localAddress(),1);CHECK(first&&peer);
    ExposedSession s{std::move(*first)};detail::RunCompletion completion{loop};Errc error=Errc::aborted;
    CHECK(s.state().beginReceive(completion.handle(),{},error));
    run(loop,sendOne(*peer,KcpMessage{'a'}));if(!completion.finished)loop.runUntilStopped(completion.finished);
    CHECK(!s.state().beginReceive(completion.handle(),{},error)&&error==Errc::busy);
    auto received=s.state().takeReceive();CHECK(received&&*received==KcpMessage({'a'}));
    run(loop,closeH(s));run(loop,closeH(*peer));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}
TEST_CASE("KCP closing one shared conv leaves its sibling operational") {
    Loop loop;auto a=KcpEndpoint::create(loop,local()),b=KcpEndpoint::create(loop,local());CHECK(a&&b);
    auto a1=a->openSession(*b->localAddress(),1),b1=b->openSession(*a->localAddress(),1);
    auto a2=a->openSession(*b->localAddress(),2),b2=b->openSession(*a->localAddress(),2);CHECK(a1&&b1&&a2&&b2);
    run(loop,closeH(*a1));run(loop,closeH(*b1));run(loop,both(*a2,*b2));
    run(loop,closeH(*a2));run(loop,closeH(*b2));run(loop,closeH(*a));run(loop,closeH(*b));drain(loop);
}
#ifdef __linux__
static std::size_t descriptors(){
    auto* directory=opendir("/proc/self/fd");CHECK(directory);std::size_t n=0;
    while(auto* e=readdir(directory))if(e->d_name[0]!='.')++n;
    closedir(directory);return n;
}
TEST_CASE("KCP repeated shutdown leaks no descriptors") {
    Loop loop;auto baseline=descriptors();
    for(int n=0;n<20;++n){auto e=KcpEndpoint::create(loop,local());CHECK(e);
        auto s=e->openSession(*SocketAddress::ipv4("127.0.0.1",9),1);CHECK(s);
        run(loop,closeH(*s));run(loop,closeH(*e));drain(loop);CHECK(descriptors()==baseline);
    }
}
#endif

#ifdef UVCO_TEST_KCP_PENDING_SENDS
TEST_CASE("KCP Loop-first destruction drains in-flight UDP sends without replacing committed success") {
    std::unique_ptr<Loop> loop(new Loop);auto a=KcpEndpoint::create(*loop,local()),b=KcpEndpoint::create(*loop,local());CHECK(a&&b);
    KcpSessionOptions o;o.core=CoreOptions::turbo();
    auto s=a->openSession(*b->localAddress(),1,o);CHECK(s);auto task=sendOne(*s,KcpMessage(65536,'x'));
    detail::RunCompletion completion{*loop};detail::co2compat::TaskAccess::start(task,completion.handle(),{});
    CHECK(s->stats().queuedPackets>0);loop.reset();CHECK(completion.finished);
    detail::co2compat::TaskAccess::takeResult(task);
}
#endif
UVCO_TEST_MAIN
#else
int main(){std::fputs("FAIL: KCP coroutine runtime missing\n",stderr);return 1;}
#endif
