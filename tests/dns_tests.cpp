#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
#include <string>
#include <vector>
namespace {
auto lookup(uvco::Loop& loop,std::string host="127.0.0.1",bool v6=false)
 CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::SocketAddress>>>,(loop,host,v6),addrinfo hints{};
 uvco::Result<std::vector<uvco::SocketAddress>> r;) {
 hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;hints.ai_socktype=SOCK_STREAM;hints.ai_family=v6?AF_INET6:AF_INET;
 CO2_AWAIT_SET(r,uvco::getaddrinfo(loop,std::move(host),std::string{"80"},hints));CO2_RETURN(std::move(r));
} CO2_END
auto reverse(uvco::Loop& loop,uvco::SocketAddress address)
 CO2_BEG(co2::Task<uvco::Result<uvco::NameInfo>>,(loop,address),uvco::Result<uvco::NameInfo> r;) {
 CO2_AWAIT_SET(r,uvco::getnameinfo(loop,address,NI_NUMERICHOST|NI_NUMERICSERV));CO2_RETURN(std::move(r));
} CO2_END
}
TEST_CASE("DNS numeric IPv4 and reverse own temporary strings"){
 uvco::Loop loop;auto r=uvco::run(loop,lookup(loop));CHECK(r);CHECK(r->size()==1);CHECK(r->front().port()==80);
 auto n=uvco::run(loop,reverse(loop,r->front()));CHECK(n);CHECK(n->host=="127.0.0.1");CHECK(n->service=="80");
}
TEST_CASE("DNS numeric IPv6 and reverse"){
 uvco::Loop loop;auto r=uvco::run(loop,lookup(loop,"::1",true));CHECK(r);CHECK(!r->empty());CHECK(r->front().family()==AF_INET6);
 auto n=uvco::run(loop,reverse(loop,r->front()));CHECK(n);CHECK(n->host=="::1");CHECK(n->service=="80");
}
TEST_CASE("DNS invalid numeric input is a Result error"){
 uvco::Loop loop;auto r=uvco::run(loop,lookup(loop,"not-a-numeric-host"));CHECK(!r);
}
TEST_CASE("DNS pre-cancel submits no request and reverse also cancels"){
 uvco::Loop loop;co2::stop_source source;source.request_stop();auto r=uvco::run(loop,lookup(loop),source.get_token());
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);
 auto a=uvco::SocketAddress::ipv4("127.0.0.1",80);CHECK(a);
 auto n=uvco::run(loop,reverse(loop,*a),source.get_token());CHECK(!n);CHECK(n.error()==uvco::Errc::canceled);
}
UVCO_TEST_MAIN
