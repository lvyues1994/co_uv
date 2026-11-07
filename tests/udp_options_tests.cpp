#include "test.hpp"
#include "uvco/uvco.hpp"
TEST_CASE("UDP multicast options and queue metrics preserve native errors"){
 uvco::Loop loop;uvco::Udp udp{loop};auto address=uvco::SocketAddress::ipv4("0.0.0.0",0);CHECK(address);CHECK(udp.bind(*address));
 CHECK(udp.setMulticastLoop(true));CHECK(udp.setMulticastTtl(1));CHECK(!udp.setMulticastTtl(256));
 CHECK(!udp.setMembership("not-an-ip","",UV_JOIN_GROUP));CHECK(!udp.setMulticastInterface("not-an-ip"));
 CHECK(udp.sendQueueSize()==0);CHECK(udp.sendQueueCount()==0);
#ifndef _WIN32
 auto r=udp.setMembership("239.255.42.99","127.0.0.1",UV_JOIN_GROUP);
 if(r){CHECK(udp.setMulticastInterface("127.0.0.1"));CHECK(udp.setMembership("239.255.42.99","127.0.0.1",UV_LEAVE_GROUP));}
 else {int e=uvco::toInt(r.error());CHECK(e==UV_ENODEV || e==UV_EADDRNOTAVAIL || e==UV_ENETUNREACH || e==UV_ENOTSUP);}
#endif
 CHECK(!udp.setSourceMembership("not-an-ip","","127.0.0.1",UV_JOIN_GROUP));
}
UVCO_TEST_MAIN
