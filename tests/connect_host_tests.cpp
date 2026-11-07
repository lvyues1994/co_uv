#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/net/connect.hpp"
#include "uvco/core/run.hpp"
TEST_CASE("numeric connect bypasses resolver and returns owned TCP") {
 uvco::Loop loop;uvco::Tcp server{loop};CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());
 int calls=0;uvco::DnsResolver resolver{loop,{},[&](uvco::Loop&,std::string,std::string,addrinfo)->co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>{++calls;return {};}};
 auto local=server.localAddress();CHECK(local);auto connected=uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",local->port()));
 CHECK(connected);CHECK(connected->peerAddress());CHECK(calls==0);
}
TEST_CASE("invalid pacing options fail without opening sockets") {
 uvco::Loop loop;uvco::DnsResolver resolver{loop};uvco::ConnectOptions options;options.minimumDelay=std::chrono::milliseconds{9};
 CHECK(uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",1,options)).error()==uvco::Errc::invalid_argument);
}
namespace {
auto addresses(addrinfo hints,std::uint16_t port)
 CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>,(hints,port),std::vector<uvco::SocketAddress> list;) {
 if(hints.ai_family==AF_INET6)list.push_back(*uvco::SocketAddress::ipv6("::1",port));
 else list.push_back(*uvco::SocketAddress::ipv4("127.0.0.1",port));
 CO2_RETURN(std::move(list));
} CO2_END
}
TEST_CASE("ordinary IPv6 refusal does not poison later IPv4 success") {
 uvco::Loop loop;uvco::Tcp server{loop};CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());
 auto local=server.localAddress();CHECK(local);auto port=local->port();std::vector<int> launches;
 uvco::DnsResolver resolver{loop,{},[port](uvco::Loop&,std::string,std::string,addrinfo h){return addresses(h,port);}};
 uvco::ConnectOptions options;options.minimumDelay=options.attemptDelay=std::chrono::milliseconds{10};
 options.connector=[&](uvco::Loop& l,uvco::SocketAddress a){launches.push_back(a.family());return uvco::detail::connectOne(l,a);};
 auto connected=uvco::run(loop,uvco::connect(loop,resolver,"numeric-fixture.test",port,options));
 CHECK(connected);CHECK(launches.size()==2);CHECK(launches[0]==AF_INET6);CHECK(launches[1]==AF_INET);
}
TEST_CASE("family preference delay and late-family interleave use deterministic policy") {
 uvco::Loop loop;uvco::DnsResolver resolver{loop};uvco::ConnectOptions options;
 options.clock=[] {return std::chrono::steady_clock::time_point{};};
 uvco::detail::ConnectRaceState s{loop,resolver,options};
 s.family[1].push_back(*uvco::SocketAddress::ipv4("127.0.0.1",1));
 CHECK(s.choose(std::chrono::steady_clock::time_point{}+std::chrono::milliseconds{49})==-1);
 CHECK(s.choose(std::chrono::steady_clock::time_point{}+std::chrono::milliseconds{50})==1);
 s.lastFamily=1;s.family[0].push_back(*uvco::SocketAddress::ipv6("::1",1));
 CHECK(s.choose(std::chrono::steady_clock::time_point{})==0);
}
TEST_CASE("pre-canceled connect starts no attempts and throwing connector is drained") {
 uvco::Loop loop;uvco::DnsResolver resolver{loop};int calls=0;co2::stop_source stop;stop.request_stop();
 uvco::ConnectOptions options;options.connector=[&](uvco::Loop&,uvco::SocketAddress)->co2::Task<uvco::Result<uvco::Tcp>> {
 ++calls;throw std::runtime_error("connector construction");};
 CHECK(uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",1,options),stop.get_token()).error()==uvco::Errc::canceled);
 CHECK(calls==0);bool threw=false;
 try{static_cast<void>(uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",1,options)));}catch(std::runtime_error const&){threw=true;}
 CHECK(threw);CHECK(calls==1);
}
namespace {
auto stalled(uvco::Loop& loop,int& cleaned)
 CO2_BEG(co2::Task<uvco::Result<uvco::Tcp>>,(loop,cleaned),uvco::Result<void> r;) {
 CO2_AWAIT_SET(r,uvco::sleep(loop,std::chrono::seconds{30}));++cleaned;
 CO2_RETURN(r?uvco::Errc::timed_out:r.error());
} CO2_END
auto gateWait(uvco::Event& gate) CO2_BEG(co2::Task<>,(gate),uvco::Result<void> r;) {
 CO2_AWAIT_SET(r,gate.wait());
} CO2_END
auto uncancelableDns(uvco::Event& gate,bool& finished)
 CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>,(gate,finished)) {
 CO2_AWAIT((uvco::detail::ConnectGroupAwaiter{gateWait(gate),{}}));finished=true;
 CO2_RETURN(std::vector<uvco::SocketAddress>{});
} CO2_END
}
TEST_CASE("a successful family cancels and drains a blackhole loser") {
 uvco::Loop loop;uvco::Tcp server{loop};CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());
 auto local=server.localAddress();CHECK(local);auto port=local->port();int cleaned=0;
 uvco::DnsResolver resolver{loop,{},[port](uvco::Loop&,std::string,std::string,addrinfo h){return addresses(h,port);}};
 uvco::ConnectOptions options;options.minimumDelay=options.attemptDelay=std::chrono::milliseconds{10};
 options.connector=[&](uvco::Loop& l,uvco::SocketAddress a){return a.isIpv6()?stalled(l,cleaned):uvco::detail::connectOne(l,a);};
 CHECK(uvco::run(loop,uvco::connect(loop,resolver,"fixture.test",port,options)));CHECK(cleaned==1);
}
TEST_CASE("per-attempt timeout and in-flight parent cancel both drain callable cleanup") {
 uvco::Loop loop;uvco::DnsResolver resolver{loop};int cleaned=0;uvco::ConnectOptions options;
 options.perAttemptTimeout=std::chrono::milliseconds{10};options.connector=[&](uvco::Loop& l,uvco::SocketAddress){return stalled(l,cleaned);};
 CHECK(uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",1,options)).error()==uvco::Errc::timed_out);CHECK(cleaned==1);
 co2::stop_source stop;options.connector=[&](uvco::Loop& l,uvco::SocketAddress){stop.request_stop();return stalled(l,cleaned);};
 CHECK(uvco::run(loop,uvco::connect(loop,resolver,"127.0.0.1",1,options),stop.get_token()).error()==uvco::Errc::canceled);CHECK(cleaned==2);
}
TEST_CASE("winning TCP does not wait for an independently owned uncancelable DNS producer") {
 uvco::Loop loop;uvco::Event gate{loop};bool finished=false;uvco::Tcp server{loop};
 CHECK(server.bind(*uvco::SocketAddress::ipv4("127.0.0.1",0)));CHECK(server.listen());auto local=server.localAddress();CHECK(local);auto port=local->port();
 uvco::DnsResolver resolver{loop,{},[&](uvco::Loop&,std::string,std::string,addrinfo h){return h.ai_family==AF_INET6?uncancelableDns(gate,finished):addresses(h,port);}};
 auto connected=uvco::run(loop,uvco::connect(loop,resolver,"fixture.test",port));CHECK(connected);CHECK(!finished);
 gate.set();static_cast<void>(connected->close());static_cast<void>(server.close());loop.runUntilIdle();CHECK(finished);
}
UVCO_TEST_MAIN
