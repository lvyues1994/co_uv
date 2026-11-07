#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/dns/resolver.hpp"
#include "uvco/core/run.hpp"
#include "uvco/timer/sleep.hpp"
namespace {
auto lookup(uvco::DnsResolver& resolver,std::string host="127.0.0.1")
 CO2_BEG(co2::Task<uvco::Result<uvco::AddressList>>, (resolver,host), uvco::Result<uvco::AddressList> r; addrinfo hints{};) {
 hints.ai_family=AF_INET; hints.ai_flags=AI_NUMERICHOST; hints.ai_socktype=SOCK_STREAM;
 CO2_AWAIT_SET(r,resolver.resolve(std::move(host),"80",hints)); CO2_RETURN(r);
} CO2_END
}
TEST_CASE("resolver cache is opt in and invalidation separates generations") {
 uvco::Loop loop; uvco::DnsCacheOptions options; options.enabled=true;
 uvco::DnsResolver resolver{loop,options};
 auto a=uvco::run(loop,lookup(resolver)); CHECK(a && !(*a)->empty());
 auto b=uvco::run(loop,lookup(resolver)); CHECK(b && *a==*b); CHECK(resolver.cacheSize()==1);
 resolver.invalidate(); auto c=uvco::run(loop,lookup(resolver)); CHECK(c && *c!=*a);
}
TEST_CASE("default resolver does not cache completed results") {
 uvco::Loop loop; uvco::DnsResolver resolver{loop}; auto a=uvco::run(loop,lookup(resolver));
 auto b=uvco::run(loop,lookup(resolver)); CHECK(a && b); CHECK(*a!=*b); CHECK(resolver.cacheSize()==0);
}
namespace {
struct Counts {int calls=0,finished=0;bool throwing=false;};
auto fake(uvco::Loop& loop,std::shared_ptr<Counts> c)
 CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>,(loop,c),uvco::Result<void> r;std::vector<uvco::SocketAddress> a;) {
 CO2_AWAIT_SET(r,uvco::sleep(loop,std::chrono::milliseconds{1})); ++c->finished;
 if(c->throwing)throw std::runtime_error("DNS backend");
 if(!r)CO2_RETURN(r.error());
 a.push_back(*uvco::SocketAddress::ipv4("127.0.0.1",80)); CO2_RETURN(std::move(a));
} CO2_END
uvco::DnsQuery provider(std::shared_ptr<Counts> c) {
 return [c](uvco::Loop& l,std::string,std::string,addrinfo){++c->calls;return fake(l,c);};
}
}
TEST_CASE("single flight subscriber cancellation does not cancel another subscriber") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();uvco::DnsResolver resolver{loop,{},provider(counts)};
 auto a=lookup(resolver),b=lookup(resolver);co2::stop_source stop;
 uvco::detail::RunCompletion ca{loop},cb{loop};
 uvco::detail::co2compat::TaskAccess::start(a,ca.handle(),stop.get_token());
 uvco::detail::co2compat::TaskAccess::start(b,cb.handle(),{});stop.request_stop();loop.runUntilIdle();
 CHECK(ca.finished && cb.finished);CHECK(counts->calls==1);CHECK(counts->finished==1);
 CHECK(uvco::detail::co2compat::TaskAccess::takeResult(a).error()==uvco::Errc::canceled);
 CHECK(uvco::detail::co2compat::TaskAccess::takeResult(b));
}
TEST_CASE("Loop-first resolver teardown aborts subscriber and drains independent producer") {
 auto loop=std::unique_ptr<uvco::Loop>{new uvco::Loop};auto counts=std::make_shared<Counts>();
 uvco::DnsResolver resolver{*loop,{},provider(counts)};auto task=lookup(resolver);
 uvco::detail::RunCompletion complete{*loop};uvco::detail::co2compat::TaskAccess::start(task,complete.handle(),{});
 loop.reset();CHECK(complete.finished);CHECK(counts->finished==1);
 CHECK(uvco::detail::co2compat::TaskAccess::takeResult(task).error()==uvco::Errc::aborted);
}
TEST_CASE("large local maxAge never overflows steady-clock arithmetic") {
 uvco::Loop loop;uvco::DnsCacheOptions options;options.enabled=true;options.maxAge=std::chrono::milliseconds::max();
 auto now=std::chrono::steady_clock::time_point{};auto counts=std::make_shared<Counts>();
 uvco::DnsResolver resolver{loop,options,provider(counts),[&]{return now;}};
 auto a=uvco::run(loop,lookup(resolver));now+=std::chrono::milliseconds{1};auto b=uvco::run(loop,lookup(resolver));
 CHECK(a && b);CHECK(*a==*b);CHECK(counts->calls==1);
}
TEST_CASE("resolver backend exceptions drain before rethrow and bounds return busy") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();counts->throwing=true;
 uvco::DnsResolver resolver{loop,{},provider(counts)};bool threw=false;
 try{static_cast<void>(uvco::run(loop,lookup(resolver)));}catch(std::runtime_error const&){threw=true;}
 CHECK(threw);CHECK(counts->finished==1);
 uvco::DnsCacheOptions options;options.maxInFlight=0;uvco::DnsResolver bounded{loop,options};
 CHECK(uvco::run(loop,lookup(bounded)).error()==uvco::Errc::busy);
}

TEST_CASE("resolver query-factory close cannot launch a producer on a closing resident") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();uvco::DnsResolver* owner=nullptr;
 uvco::DnsResolver resolver{loop,{},[&](uvco::Loop& l,std::string,std::string,addrinfo){static_cast<void>(owner->close());return fake(l,counts);}};
 owner=&resolver;CHECK(uvco::run(loop,lookup(resolver)).error()==uvco::Errc::aborted);
 CHECK(counts->finished==0);
}
TEST_CASE("resolver cache clock reentrant invalidation cannot republish an old generation") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();uvco::DnsCacheOptions options;options.enabled=true;
 uvco::DnsResolver* owner=nullptr;int clocks=0;
 uvco::DnsResolver resolver{loop,options,provider(counts),[&]{if(++clocks==2)owner->invalidate();return std::chrono::steady_clock::time_point{};}};
 owner=&resolver;CHECK(uvco::run(loop,lookup(resolver)));CHECK(clocks==2);CHECK(resolver.cacheSize()==0);
}

TEST_CASE("resolver LRU entry bounds and local age eviction are deterministic") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();uvco::DnsCacheOptions options;options.enabled=true;options.maxEntries=2;
 options.maxAge=std::chrono::milliseconds{2};auto now=std::chrono::steady_clock::time_point{};
 uvco::DnsResolver resolver{loop,options,provider(counts),[&]{return now;}};
 auto a=uvco::run(loop,lookup(resolver));auto b=uvco::run(loop,lookup(resolver,"127.0.0.2"));CHECK(a && b);
 CHECK(*uvco::run(loop,lookup(resolver))==*a);CHECK(uvco::run(loop,lookup(resolver,"127.0.0.3")));
 CHECK(resolver.cacheSize()==2);auto again=uvco::run(loop,lookup(resolver,"127.0.0.2"));CHECK(again && *again!=*b);CHECK(counts->calls==4);
 now+=std::chrono::milliseconds{2};auto expired=uvco::run(loop,lookup(resolver));CHECK(expired && *expired!=*a);CHECK(counts->calls==5);
 CHECK(resolver.cacheSize()==1);
}
TEST_CASE("resolver byte budget skips oversized entries and subscriber budget refuses excess") {
 uvco::Loop loop;auto counts=std::make_shared<Counts>();uvco::DnsCacheOptions options;options.enabled=true;options.maxBytes=1;
 uvco::DnsResolver tiny{loop,options,provider(counts)};auto a=uvco::run(loop,lookup(tiny));auto b=uvco::run(loop,lookup(tiny));
 CHECK(a && b && *a!=*b);CHECK(tiny.cacheSize()==0);CHECK(counts->calls==2);
 options.maxSubscribers=1;uvco::DnsResolver bounded{loop,options,provider(counts)};auto first=lookup(bounded),second=lookup(bounded);
 uvco::detail::RunCompletion ca{loop},cb{loop};
 uvco::detail::co2compat::TaskAccess::start(first,ca.handle(),{});
 uvco::detail::co2compat::TaskAccess::start(second,cb.handle(),{});
 CHECK(cb.finished);CHECK(uvco::detail::co2compat::TaskAccess::takeResult(second).error()==uvco::Errc::busy);
 loop.runUntilIdle();CHECK(ca.finished);CHECK(uvco::detail::co2compat::TaskAccess::takeResult(first));
}
UVCO_TEST_MAIN
