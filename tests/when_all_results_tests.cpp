#include <co2/co2.hpp>
#include <tuple>
#include <memory>
#include <stdexcept>
#include <vector>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
auto branch(uvco::Loop& loop,int n,int yields,bool& ended,bool& stopped,bool throws=false)
 CO2_BEG(co2::Task<uvco::Result<int>>, (loop,n,yields,ended,stopped,throws),int i=0;co2::stop_token token;) {
 CO2_AWAIT_SET(token,co2::getStopToken());
 for(i=0;i<yields;++i)CO2_AWAIT(co2::scheduleOn(loop));
 ended=true;stopped=token.stop_requested();
 if(throws)throw std::runtime_error{"body"};
 if(n<0)CO2_RETURN(uvco::Result<int>{uvco::Errc::invalid_argument});
 CO2_RETURN(uvco::Result<int>{n});
} CO2_END
auto noop() CO2_BEG(co2::Task<void>, ()) {} CO2_END
auto unique() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
 CO2_RETURN(std::unique_ptr<int>{new int{9}});
} CO2_END
using Values=std::tuple<int,std::unique_ptr<int>>;
auto good(uvco::Loop& loop,bool& a,bool& b)
 CO2_BEG(co2::Task<uvco::Result<Values>>, (loop,a,b),uvco::Result<Values> r;) {
 CO2_AWAIT_SET(r,uvco::whenAllResults(branch(loop,7,2,a,b),noop(),unique()));
 CO2_RETURN(std::move(r));
} CO2_END
using Ints=std::tuple<int,int>;
auto failing(uvco::Loop& loop,bool& a,bool& sa,bool& b,bool& sb,bool ex=false)
 CO2_BEG(co2::Task<uvco::Result<Ints>>, (loop,a,sa,b,sb,ex),uvco::Result<Ints> r;) {
 CO2_AWAIT_SET(r,uvco::whenAllResults(branch(loop,4,8,a,sa),branch(loop,-1,2,b,sb,ex)));
 CO2_RETURN(std::move(r));
} CO2_END
auto ranged(uvco::Loop& loop,bool empty,bool& a,bool& sa,bool& b,bool& sb)
 CO2_BEG(co2::Task<uvco::Result<std::vector<int>>>, (loop,empty,a,sa,b,sb),
 std::vector<co2::Task<uvco::Result<int>>> tasks;uvco::Result<std::vector<int>> r;) {
 if(!empty){tasks.push_back(branch(loop,1,1,a,sa));tasks.push_back(branch(loop,2,4,b,sb));}
 CO2_AWAIT_SET(r,uvco::whenAllResults(std::move(tasks)));CO2_RETURN(std::move(r));
} CO2_END
} // namespace
TEST_CASE("allResults unwraps heterogeneous values and omits void"){
 uvco::Loop loop;bool a=false,s=false;auto r=uvco::run(loop,good(loop,a,s));
 CHECK(r);CHECK(std::get<0>(*r)==7);CHECK(*std::get<1>(*r)==9);CHECK(a);CHECK(!s);
}
TEST_CASE("allResults first error stops and drains siblings"){
 uvco::Loop loop;bool a=false,sa=false,b=false,sb=false;
 auto r=uvco::run(loop,failing(loop,a,sa,b,sb));CHECK(!r);CHECK(r.error()==uvco::Errc::invalid_argument);
 CHECK(a);CHECK(b);CHECK(sa);CHECK(!sb);
}
TEST_CASE("allResults exception stops and drains before rethrow"){
 uvco::Loop loop;bool a=false,sa=false,b=false,sb=false,caught=false;
 try{static_cast<void>(uvco::run(loop,failing(loop,a,sa,b,sb,true)));}catch(std::runtime_error const&){caught=true;}
 CHECK(caught);CHECK(a);CHECK(b);CHECK(sa);
}
TEST_CASE("allResults range preserves order and accepts empty"){
 uvco::Loop loop;bool a=false,sa=false,b=false,sb=false;
 auto r=uvco::run(loop,ranged(loop,false,a,sa,b,sb));CHECK(r);CHECK(*r==std::vector<int>({1,2}));CHECK(!sa);CHECK(!sb);
 auto e=uvco::run(loop,ranged(loop,true,a,sa,b,sb));CHECK(e);CHECK(e->empty());
}

namespace {
struct ThrowMove {
 int stage=0;
 ThrowMove()=default;
 ThrowMove(ThrowMove const&)=delete;
 ThrowMove(ThrowMove&& other):stage{other.stage+1}{if(other.stage>=1)throw std::runtime_error{"transfer"};}
 ThrowMove& operator=(ThrowMove&& other){if(other.stage>=1)throw std::runtime_error{"transfer"};stage=other.stage+1;return *this;}
};
auto throwingValue(uvco::Loop& loop) CO2_BEG(co2::Task<ThrowMove>,(loop)) {
 CO2_AWAIT(co2::scheduleOn(loop));CO2_RETURN(ThrowMove{});
} CO2_END
using ThrowValues=std::tuple<ThrowMove,int>;
auto moveAll(uvco::Loop& loop,bool& ended,bool& stopped)
 CO2_BEG(co2::Task<uvco::Result<ThrowValues>>,(loop,ended,stopped),uvco::Result<ThrowValues> r;) {
 CO2_AWAIT_SET(r,uvco::whenAllResults(throwingValue(loop),branch(loop,1,8,ended,stopped)));
 CO2_RETURN(std::move(r));
} CO2_END
auto voidRange() CO2_BEG(co2::Task<uvco::Result<void>>,(),std::vector<co2::Task<void>> tasks;uvco::Result<void> r;) {
 tasks.push_back(noop());tasks.push_back(noop());CO2_AWAIT_SET(r,uvco::whenAllResults(std::move(tasks)));CO2_RETURN(r);
} CO2_END
}
TEST_CASE("allResults transfer exception drains pending siblings"){
 uvco::Loop loop;bool ended=false,stopped=false,caught=false;
 try{static_cast<void>(uvco::run(loop,moveAll(loop,ended,stopped)));}catch(std::runtime_error const& e){caught=std::string{e.what()}=="transfer";}
 CHECK(caught);CHECK(ended);CHECK(stopped);
}
TEST_CASE("allResults pre-cancel does not start children"){
 uvco::Loop loop;co2::stop_source source;source.request_stop();bool a=false,sa=false,b=false,sb=false;
 auto r=uvco::run(loop,failing(loop,a,sa,b,sb),source.get_token());CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(!a);CHECK(!b);
}
TEST_CASE("allResults void range") {uvco::Loop loop;CHECK(uvco::run(loop,voidRange()));}
UVCO_TEST_MAIN

