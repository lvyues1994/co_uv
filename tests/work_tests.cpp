#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
template<class Fn> auto invoke(uvco::Loop& loop,Fn fn)
 CO2_BEG(co2::Task<uvco::Result<typename std::decay<decltype(fn())>::type>>, (loop,fn),
 uvco::Result<typename std::decay<decltype(fn())>::type> r;) {
 CO2_AWAIT_SET(r,uvco::queueWork(loop,std::move(fn)));CO2_RETURN(std::move(r));
} CO2_END
struct Poll {
 std::promise<void>* started;std::atomic<bool>* done;
 int operator()(co2::stop_token token) const {
  started->set_value();while(!token.stop_requested())std::this_thread::yield();done->store(true,std::memory_order_release);return 9;
 }
};
auto polling(uvco::Loop& loop,Poll fn) CO2_BEG(co2::Task<uvco::Result<int>>,(loop,fn),uvco::Result<int> r;) {
 CO2_AWAIT_SET(r,uvco::queueWork(loop,fn));CO2_RETURN(r);
} CO2_END
struct Block {std::shared_future<void> gate;bool done=false;};
struct Count {std::atomic<bool>* called;int operator()() const {called->store(true);return 1;}};
auto queued(uvco::Loop& loop,std::atomic<bool>& called,std::promise<void>& release)
 CO2_BEG(co2::Task<uvco::Result<int>>,(loop,called,release),uvco::Result<int> r;) {
 CO2_AWAIT_SET(r,uvco::queueWork(loop,Count{&called}));
 release.set_value();CO2_RETURN(r);
} CO2_END
struct ThrowMove {
 ThrowMove()=default;ThrowMove(ThrowMove const&)=delete;
 ThrowMove(ThrowMove&&){throw std::runtime_error{"result"};}
 ThrowMove& operator=(ThrowMove&&){throw std::runtime_error{"result"};}
};
}
TEST_CASE("Work delivers value and move-only callable on loop") {
 uvco::Loop loop;auto caller=std::this_thread::get_id();std::thread::id worker;
 auto r=uvco::run(loop,invoke(loop,[p=std::unique_ptr<int>{new int{42}},&worker]{worker=std::this_thread::get_id();return *p;}));
 CHECK(r);CHECK(*r==42);CHECK(worker!=caller);CHECK(loop.isLoopThread());
}
TEST_CASE("Work captures body and result-construction exceptions") {
 uvco::Loop loop;bool caught=false;
 try{static_cast<void>(uvco::run(loop,invoke(loop,[]()->int{throw std::runtime_error{"body"};})));}catch(std::runtime_error const&){caught=true;}
 CHECK(caught);caught=false;
 try{static_cast<void>(uvco::run(loop,invoke(loop,[]{return ThrowMove{};})));}catch(std::runtime_error const&){caught=true;}
 CHECK(caught);
}
TEST_CASE("Work running cancellation polls token and drains after-work") {
 uvco::Loop loop;co2::stop_source source;std::promise<void> started;auto ready=started.get_future();std::atomic<bool> done{false};
 std::thread stopper{[&]{ready.wait();source.request_stop();}};
 auto r=uvco::run(loop,polling(loop,Poll{&started,&done}),source.get_token());stopper.join();
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(done.load(std::memory_order_acquire));
}
TEST_CASE("Work queued cancellation never invokes callable") {
 uvco::Loop loop;std::promise<void> release;Block block{release.get_future().share(),false};uv_work_t blocker{};blocker.data=&block;
 CHECK(uv_queue_work(loop.raw(),&blocker,[](uv_work_t* req){static_cast<Block*>(req->data)->gate.wait();},
 [](uv_work_t* req,int){static_cast<Block*>(req->data)->done=true;})==0);
 co2::stop_source source;uv_timer_t timer{};CHECK(uv_timer_init(loop.raw(),&timer)==0);timer.data=&source;
 CHECK(uv_timer_start(&timer,[](uv_timer_t* h){static_cast<co2::stop_source*>(h->data)->request_stop();uv_close(reinterpret_cast<uv_handle_t*>(h),nullptr);},0,0)==0);
 std::atomic<bool> called{false};auto r=uvco::run(loop,queued(loop,called,release),source.get_token());loop.runUntilIdle();
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(!called.load());CHECK(block.done);
}
TEST_CASE("Work pre-cancel never submits callable") {
 uvco::Loop loop;co2::stop_source source;source.request_stop();bool called=false;
 auto r=uvco::run(loop,invoke(loop,[&called]{called=true;return 1;}),source.get_token());CHECK(!r);CHECK(!called);
}
TEST_CASE("Work void callable completes normally") {
 uvco::Loop loop;std::atomic<bool> called{false};auto r=uvco::run(loop,invoke(loop,[&called]{called.store(true);}));
 CHECK(r);CHECK(called.load());
}
TEST_CASE("Work cancellation after side effect still drains real completion") {
 uvco::Loop loop;co2::stop_source source;std::atomic<bool> called{false};
 auto r=uvco::run(loop,invoke(loop,[&]{called.store(true);source.request_stop();return 8;}),source.get_token());
 CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);CHECK(called.load());
}
int main(){uv_os_setenv("UV_THREADPOOL_SIZE","1");return uvco_test::runAll();}
