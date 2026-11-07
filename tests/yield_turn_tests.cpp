#include <co2/co2.hpp>
#include <memory>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
struct Context {int turn=0;co2::coroutine_handle<> waiter;};
void fire(uv_handle_t* h){auto& c=*static_cast<Context*>(h->data);uv_close(h,nullptr);auto w=std::exchange(c.waiter,{});if(w)w.resume();}
struct Phase {Context* c;bool await_ready()const{return false;}void await_suspend(co2::coroutine_handle<> h){c->waiter=h;}void await_resume()const{}};
auto probe(uvco::Loop& loop,Context& ctx) CO2_BEG(co2::Task<bool>,(loop,ctx),int before=0;uvco::Result<void> r;) {
 CO2_AWAIT(Phase{&ctx});before=ctx.turn;
 CO2_AWAIT_SET(r,uvco::yieldNextTurn(loop));CO2_RETURN(r && ctx.turn>before);
} CO2_END
}
TEST_CASE("yieldNextTurn crosses an iteration from timer idle prepare check and async phases"){
 for(int phase=0;phase<5;++phase){
  uvco::Loop loop;Context ctx;uv_prepare_t counter{},prepare{};uv_check_t check{};uv_idle_t driver{},idle{};uv_timer_t timer{};uv_async_t async{};
  CHECK(uv_prepare_init(loop.raw(),&counter)==0);counter.data=&ctx;CHECK(uv_prepare_start(&counter,[](uv_prepare_t* h){++static_cast<Context*>(h->data)->turn;})==0);
  CHECK(uv_idle_init(loop.raw(),&driver)==0);CHECK(uv_idle_start(&driver,[](uv_idle_t*){})==0);
  uv_handle_t* trigger=nullptr;
  if(phase==0){CHECK(uv_timer_init(loop.raw(),&timer)==0);timer.data=&ctx;trigger=reinterpret_cast<uv_handle_t*>(&timer);CHECK(uv_timer_start(&timer,[](uv_timer_t* h){fire(reinterpret_cast<uv_handle_t*>(h));},0,0)==0);}
  if(phase==1){CHECK(uv_idle_init(loop.raw(),&idle)==0);idle.data=&ctx;trigger=reinterpret_cast<uv_handle_t*>(&idle);CHECK(uv_idle_start(&idle,[](uv_idle_t* h){fire(reinterpret_cast<uv_handle_t*>(h));})==0);}
  if(phase==2){CHECK(uv_prepare_init(loop.raw(),&prepare)==0);prepare.data=&ctx;trigger=reinterpret_cast<uv_handle_t*>(&prepare);CHECK(uv_prepare_start(&prepare,[](uv_prepare_t* h){fire(reinterpret_cast<uv_handle_t*>(h));})==0);}
  if(phase==3){CHECK(uv_check_init(loop.raw(),&check)==0);check.data=&ctx;trigger=reinterpret_cast<uv_handle_t*>(&check);CHECK(uv_check_start(&check,[](uv_check_t* h){fire(reinterpret_cast<uv_handle_t*>(h));})==0);}
  if(phase==4){CHECK(uv_async_init(loop.raw(),&async,[](uv_async_t* h){fire(reinterpret_cast<uv_handle_t*>(h));})==0);async.data=&ctx;trigger=reinterpret_cast<uv_handle_t*>(&async);CHECK(uv_async_send(&async)==0);}
  CHECK(uvco::run(loop,probe(loop,ctx)));CHECK(trigger!=nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&counter),nullptr);uv_close(reinterpret_cast<uv_handle_t*>(&driver),nullptr);loop.runUntilIdle();
 }
}

namespace {auto yielding(uvco::Loop& loop) CO2_BEG(co2::Task<uvco::Result<void>>,(loop),uvco::Result<void> r;) {
 CO2_AWAIT_SET(r,uvco::yieldNextTurn(loop));CO2_RETURN(r);
} CO2_END}
TEST_CASE("yieldNextTurn pre-cancel and Loop teardown close both handles"){
 {uvco::Loop loop;co2::stop_source stop;stop.request_stop();auto r=uvco::run(loop,yielding(loop),stop.get_token());CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);}
 auto loop=std::unique_ptr<uvco::Loop>{new uvco::Loop};auto h=co2::spawn(*loop,yielding(*loop));uv_run(loop->raw(),UV_RUN_NOWAIT);loop.reset();
 CHECK(h.isReady());auto r=h.join();CHECK(!r);CHECK(r.error()==uvco::Errc::aborted);
}
UVCO_TEST_MAIN

