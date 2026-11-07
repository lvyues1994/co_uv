#include <type_traits>
#include "test.hpp"
#include "uvco/fs/fs_poll.hpp"
#include "uvco/detail/watcher/event_port.hpp"
using ExpectedPort = uvco::detail::watcher::EventPort<uvco::fs::FsPollChange,
    uvco::detail::watcher::BoundedFifo<uvco::fs::FsPollChange>>;
static_assert(std::is_base_of<ExpectedPort,uvco::fs::detail::FsPollBlock>::value,
              "FsPoll must reuse the shared EventPort, not a private waiter state machine");
TEST_CASE("FsPoll new epoch keeps a failed interval update transactional") {
    uvco::Loop loop;
    auto watcher=uvco::fs::FsPoll::start(loop,__FILE__,std::chrono::milliseconds{20}); CHECK(watcher);
    CHECK(!watcher->setInterval(std::chrono::milliseconds{0}));
    CHECK(watcher->interval()==std::chrono::milliseconds{20}); CHECK(watcher->isActive());
    CHECK(watcher->setInterval(std::chrono::milliseconds{30}));
    CHECK(watcher->queued()==0); CHECK(watcher->overflowCount()==0); CHECK(watcher->stop());
}
UVCO_TEST_MAIN
