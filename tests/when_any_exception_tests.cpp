#include <algorithm>
#include <atomic>
#include <thread>
#include <climits>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"

namespace {
struct MoveFailure : std::runtime_error {
 explicit MoveFailure(char const* message):std::runtime_error{message}{}
};
struct State {
 int alive=0;
 int moves=0;
 char const* failure="move";
 bool nonStd=false;
 int throwStage=1; // stage 0 -> 1 publishes into Task; stage 1 -> 2 collects it
 bool throwConstructor=false;
 bool finished=false;
 bool canceled=false;
};
struct Payload {
 State* state;
 int stage=0;
 explicit Payload(State& s):state{&s} {
  if(s.throwConstructor)throw MoveFailure{"construct"};
  ++s.alive;
 }
 Payload(Payload const&)=delete;
 Payload& operator=(Payload const&)=delete;
 Payload(Payload&& other):state{other.state},stage{other.stage+1} {
  ++state->moves;
  if(other.stage>=state->throwStage){if(state->nonStd)throw 42;throw MoveFailure{state->failure};}
  ++state->alive;
 }
 Payload& operator=(Payload&& other) {
  ++other.state->moves;
  if(other.stage>=other.state->throwStage){if(other.state->nonStd)throw 42;throw MoveFailure{other.state->failure};}
  --state->alive;state=other.state;stage=other.stage+1;++state->alive;return *this;
 }
 ~Payload(){--state->alive;}
};

auto value(uvco::Loop& loop,State& state,int yields,State* arm=nullptr)
 CO2_BEG(co2::Task<Payload>, (loop,state,yields,arm),int i=0;co2::stop_token token;) {
 CO2_AWAIT_SET(token,co2::getStopToken());
 for(i=0;i<yields;++i)CO2_AWAIT(co2::scheduleOn(loop));
 state.canceled=token.stop_requested();state.finished=true;
 if(arm)arm->throwStage=0;
 CO2_RETURN(Payload{state});
} CO2_END

auto peer(uvco::Loop& loop,State& state,int yields,State* arm=nullptr,bool throwBody=false)
 CO2_BEG(co2::Task<int>, (loop,state,yields,arm,throwBody),int i=0;co2::stop_token token;) {
 CO2_AWAIT_SET(token,co2::getStopToken());
 for(i=0;i<yields;++i)CO2_AWAIT(co2::scheduleOn(loop));
 state.canceled=token.stop_requested();state.finished=true;
 if(arm)arm->throwStage=0;
 if(throwBody)throw MoveFailure{"body"};
 CO2_RETURN(7);
} CO2_END

using TupleValue=uvco::WhenAnyResult<Payload,int>;
auto tupleRace(uvco::Loop& loop,State& payload,State& other,bool losing,bool delivery=false)
 CO2_BEG(co2::Task<int>, (loop,payload,other,losing,delivery),uvco::Result<TupleValue> r{uvco::Errc::canceled};) {
 CO2_AWAIT_SET(r,uvco::whenAny(value(loop,payload,losing?8:2),
                  peer(loop,other,losing?2:8,delivery?&payload:nullptr)));
 CO2_RETURN(r?0:-1);
} CO2_END

using RangeValue=uvco::WhenAnyRangeResult<Payload>;
auto rangeRace(uvco::Loop& loop,State& bad,State& good,bool losing,bool delivery=false)
 CO2_BEG(co2::Task<int>, (loop,bad,good,losing,delivery),std::vector<co2::Task<Payload>> tasks;
 uvco::Result<RangeValue> r{uvco::Errc::canceled};) {
 tasks.push_back(value(loop,bad,losing?8:2));
 tasks.push_back(value(loop,good,losing?2:8,delivery?&bad:nullptr));
 CO2_AWAIT_SET(r,uvco::whenAny(std::move(tasks)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto nested(uvco::Loop& loop,State& payload,State& inner,State& outer)
 CO2_BEG(co2::Task<int>, (loop,payload,inner,outer),uvco::Result<uvco::WhenAnyResult<int,int>> r;) {
 CO2_AWAIT_SET(r,uvco::whenAny(tupleRace(loop,payload,inner,false),peer(loop,outer,16)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto timedRace(uvco::Loop& loop,State& payload,State& other)
 CO2_BEG(co2::Task<int>, (loop,payload,other),uvco::Result<int> r;) {
 CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::seconds{10},tupleRace(loop,payload,other,false)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto timeoutLoser(uvco::Loop& loop,State& payload)
 CO2_BEG(co2::Task<int>, (loop,payload),uvco::Result<Payload> r{uvco::Errc::canceled};) {
 // The timer wins synchronously on the first loop iteration. The operation
 // deliberately ignores stop and still publishes a throwing-move result.
 CO2_AWAIT_SET(r,uvco::timeout(loop,std::chrono::milliseconds{0},value(loop,payload,16)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto bodyRace(uvco::Loop& loop,State& body,State& other,bool losing)
 CO2_BEG(co2::Task<int>, (loop,body,other,losing),uvco::Result<uvco::WhenAnyResult<int,int>> r;) {
 CO2_AWAIT_SET(r,uvco::whenAny(peer(loop,body,losing?8:2,nullptr,true),peer(loop,other,losing?2:8)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto wrappedValue(uvco::Loop& loop,State& state)
 CO2_BEG(co2::Task<uvco::Result<Payload>>, (loop,state)) {
 CO2_AWAIT(co2::scheduleOn(loop));
 state.finished=true;
 CO2_RETURN(uvco::Result<Payload>{Payload{state}});
} CO2_END

auto wrappedRace(uvco::Loop& loop,State& state,State& other)
 CO2_BEG(co2::Task<int>, (loop,state,other),uvco::Result<TupleValue> r{uvco::Errc::canceled};) {
 CO2_AWAIT_SET(r,uvco::whenAny(wrappedValue(loop,state),peer(loop,other,8)));
 CO2_RETURN(r?0:-1);
} CO2_END

auto poolValue(co2::ThreadPool& pool,State& state,std::atomic<int>& ready)
 CO2_BEG(co2::Task<Payload>, (pool,state,ready)) {
 CO2_AWAIT(co2::scheduleOn(pool));
 ready.fetch_add(1,std::memory_order_acq_rel);
 while(ready.load(std::memory_order_acquire)<2)std::this_thread::yield();
 state.finished=true;
 CO2_RETURN(Payload{state});
} CO2_END
using DoubleValue=uvco::WhenAnyResult<Payload,Payload>;
auto poolRace(co2::ThreadPool& pool,State& first,State& second,std::atomic<int>& ready)
 CO2_BEG(co2::Task<int>, (pool,first,second,ready),uvco::Result<DoubleValue> r{uvco::Errc::canceled};) {
 CO2_AWAIT_SET(r,uvco::whenAny(poolValue(pool,first,ready),poolValue(pool,second,ready)));
 CO2_RETURN(r?0:-1);
} CO2_END

template<class Task> void catches(Task task,uvco::Loop& loop,char const* message) {
 bool caught=false;
 try {static_cast<void>(uvco::run(loop,std::move(task)));}
 catch(MoveFailure const& e){caught=std::string{e.what()}==message;}
 CHECK(caught);
}
void clean(State const& state) {CHECK(state.finished);CHECK(state.alive==0);}
} // namespace

TEST_CASE("tuple winning collection throws after draining canceled loser") {
 uvco::Loop loop;State bad,other;
 catches(tupleRace(loop,bad,other,false),loop,"move");clean(bad);clean(other);
 CHECK(bad.moves>=2);CHECK(other.canceled);
}
TEST_CASE("tuple losing collection exception beats successful winner") {
 uvco::Loop loop;State bad,other;
 catches(tupleRace(loop,bad,other,true),loop,"move");clean(bad);clean(other);CHECK(bad.canceled);
}
TEST_CASE("vector winning collection throws after draining canceled loser") {
 uvco::Loop loop;State bad,good;good.throwStage=INT_MAX;
 catches(rangeRace(loop,bad,good,false),loop,"move");clean(bad);clean(good);CHECK(good.canceled);
}
TEST_CASE("vector losing collection exception beats successful winner") {
 uvco::Loop loop;State bad,good;good.throwStage=INT_MAX;
 catches(rangeRace(loop,bad,good,true),loop,"move");clean(bad);clean(good);CHECK(bad.canceled);
}
TEST_CASE("nested whenAny propagates move exception after both races drain") {
 uvco::Loop loop;State bad,inner,outer;
 catches(nested(loop,bad,inner,outer),loop,"move");clean(bad);clean(inner);clean(outer);
 CHECK(inner.canceled);CHECK(outer.canceled);
}
TEST_CASE("timeout around whenAny preserves move exception and cleanup") {
 uvco::Loop loop;State bad,other;
 catches(timedRace(loop,bad,other),loop,"move");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("timeout losing operation move exception beats timeout result") {
 uvco::Loop loop;State bad;
 catches(timeoutLoser(loop,bad),loop,"move");clean(bad);CHECK(bad.canceled);
}
TEST_CASE("tuple delivery construction throws only after cleanup") {
 uvco::Loop loop;State bad,other;bad.throwStage=INT_MAX;
 catches(tupleRace(loop,bad,other,false,true),loop,"move");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("vector delivery construction throws only after cleanup") {
 uvco::Loop loop;State bad,good;bad.throwStage=INT_MAX;good.throwStage=INT_MAX;
 catches(rangeRace(loop,bad,good,false,true),loop,"move");clean(bad);clean(good);CHECK(good.canceled);
}
TEST_CASE("branch publication construction exception drains race") {
 uvco::Loop loop;State bad,other;bad.throwStage=0;
 catches(tupleRace(loop,bad,other,false),loop,"move");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("branch value constructor exception drains race") {
 uvco::Loop loop;State bad,other;bad.throwConstructor=true;
 catches(tupleRace(loop,bad,other,false),loop,"construct");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("branch body exceptions retain winner and loser precedence") {
 uvco::Loop loop;
 for(bool losing:{false,true}){
  State body,other;catches(bodyRace(loop,body,other,losing),loop,"body");clean(body);clean(other);
  CHECK(losing?body.canceled:other.canceled);
 }
}

TEST_CASE("tuple slot construction exception drains peers") {
 uvco::Loop loop;State bad,other;bad.throwStage=2;
 catches(tupleRace(loop,bad,other,false),loop,"move");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("vector slot construction exception drains peers") {
 uvco::Loop loop;State bad,good;bad.throwStage=2;good.throwStage=INT_MAX;
 catches(rangeRace(loop,bad,good,false),loop,"move");clean(bad);clean(good);CHECK(good.canceled);
}
TEST_CASE("Result wrapped payload collection exception drains peers") {
 uvco::Loop loop;State bad,other;bad.throwStage=2;
 catches(wrappedRace(loop,bad,other),loop,"move");clean(bad);clean(other);CHECK(other.canceled);
}
TEST_CASE("concurrent branch move exceptions publish independent exception slots") {
 co2::ThreadPool pool{2};uvco::Loop loop;
 for(int i=0;i<20;++i){
  State first,second;std::atomic<int> ready{0};
  catches(poolRace(pool,first,second,ready),loop,"move");clean(first);clean(second);
 }
}
TEST_CASE("WhenAnyResult throwing move leaves valid source and empty destination") {
 State state;state.throwStage=INT_MAX;
 {
  uvco::WhenAnyResult<Payload> source,destination;
  source.emplace<0>(state);
  state.throwStage=0;bool caught=false;
  try{destination=std::move(source);}catch(MoveFailure const&){caught=true;}
  CHECK(caught);CHECK(source.index()==0);
  CHECK(destination.index()==uvco::WhenAnyResult<Payload>::npos);
 }
 CHECK(state.alive==0);
}

TEST_CASE("first collection exception is preserved while a second loser also throws") {
 uvco::Loop loop;State first,second;first.failure="first";second.failure="second";
 catches(rangeRace(loop,first,second,false),loop,"first");clean(first);clean(second);CHECK(second.canceled);
}
TEST_CASE("non std collection exception is captured without terminating") {
 uvco::Loop loop;State bad,other;bad.nonStd=true;bool caught=false;
 try{static_cast<void>(uvco::run(loop,tupleRace(loop,bad,other,false)));}catch(int n){caught=n==42;}
 CHECK(caught);clean(bad);clean(other);CHECK(other.canceled);
}

int main(int argc,char** argv) {
 // Optional case substring for individually reproducing terminate regressions.
 if(argc>1){
  auto& cases=uvco_test::registry();
  cases.erase(std::remove_if(cases.begin(),cases.end(),[&](uvco_test::Case const& c){
   return std::string{c.name}.find(argv[1])==std::string::npos;
  }),cases.end());
  if(cases.empty())return EXIT_FAILURE;
 }
 return uvco_test::runAll();
}
