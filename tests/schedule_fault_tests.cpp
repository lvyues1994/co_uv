#include <atomic>
#include <new>
#include <thread>
#include "test.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/co2_compat.hpp"
namespace {std::atomic<bool> fail{false};
struct Frame {
 uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false};int calls=0;
 static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* p){++reinterpret_cast<Frame*>(p)->calls;return nullptr;}
 co2::coroutine_handle<> handle(){return co2::coroutine_handle<>::from_address(&header);}
};}
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t n){if(fail.load())throw std::bad_alloc{};return __real__Znwm(n);}
TEST_CASE("local and remote schedule and drain allocate nothing after Loop construction") {
 uvco::Loop loop;Frame frames[128];auto alive=loop.keepAlive();std::atomic<bool> start{false};
 std::thread remote{[&]{while(!start.load())std::this_thread::yield();for(int i=64;i<128;++i)loop.schedule(frames[i].handle());alive.reset();}};
 fail.store(true);for(int i=0;i<64;++i)loop.schedule(frames[i].handle());start.store(true);loop.runUntilIdle();remote.join();fail.store(false);
 for(auto& f:frames)CHECK(f.calls==1);
}
UVCO_TEST_MAIN
