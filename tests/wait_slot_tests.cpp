#include <memory>
#include "test.hpp"
#include "uvco/core/wait_slot.hpp"
namespace {
struct Frame {
 uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false};int count=0;
 static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* h){++reinterpret_cast<Frame*>(h)->count;return nullptr;}
 co2::coroutine_handle<> handle(){return co2::coroutine_handle<>::from_address(&header);}
};
}
TEST_CASE("WaitSlot keeps reservation until take and never chooses resume policy"){
 uvco::detail::WaitSlot<int> slot;Frame f;CHECK(slot.idle());slot.begin();slot.pend(f.handle());CHECK(slot.pending());
 auto h=slot.complete(42);CHECK(slot.done());CHECK(!slot.idle());CHECK(f.count==0);h.resume();CHECK(f.count==1);
 auto r=slot.take();CHECK(r.suspended);CHECK(r.result);CHECK(*r.result==42);CHECK(slot.idle());
}
TEST_CASE("WaitSlot immediate error is not a suspended operation and is reusable"){
 uvco::detail::WaitSlot<void> slot;slot.begin();slot.set(uvco::Errc::canceled);auto r=slot.take();CHECK(!r.suspended);CHECK(!r.result);
 CHECK(r.result.error()==uvco::Errc::canceled);slot.begin();slot.set({});CHECK(slot.take().result);
}
TEST_CASE("WaitSlot supports move-only success payload"){
 uvco::detail::WaitSlot<std::unique_ptr<int>> slot;slot.begin();slot.set(std::unique_ptr<int>{new int{7}});
 auto r=slot.take();CHECK(r.result);CHECK(**r.result==7);
}
UVCO_TEST_MAIN
