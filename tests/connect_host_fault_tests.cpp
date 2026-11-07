#include <new>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/net/connect.hpp"
#include "uvco/core/run.hpp"
namespace {unsigned countdown=0;}
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t n) {
 if(countdown && --countdown==0)throw std::bad_alloc{};
 return __real__Znwm(n);
}
TEST_CASE("HE setup allocation failure disarms parent cancellation hook") {
 uvco::Loop loop;uvco::DnsResolver resolver{loop};co2::stop_source stop;uvco::ConnectOptions options;options.maxConcurrent=1;
 // Non-numeric input: slot + vector allocations precede the group frame. Its
 // third allocation fails after the parent callback was registered, before any
 // work started. The callback must be disarmed even on coroutine setup unwind.
 options.clock=[] {countdown=3;return std::chrono::steady_clock::now();};
 bool caught=false;
 try{static_cast<void>(uvco::run(loop,uvco::connect(loop,resolver,"fixture.test",1,options),stop.get_token()));}
 catch(std::bad_alloc const&){caught=true;}
 countdown=0;CHECK(caught);stop.request_stop();loop.runUntilIdle();
}
UVCO_TEST_MAIN
