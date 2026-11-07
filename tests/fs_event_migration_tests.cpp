#include "test.hpp"
#include "uvco/fs/fs_event.hpp"
TEST_CASE("FsEvent start alias preserves transactional restart and counters") {
    uvco::Loop loop;
    auto watcher=uvco::fs::FsEvent::watch(loop,__FILE__); CHECK(watcher);
    CHECK(watcher->start(__FILE__)); CHECK(watcher->isActive());
    auto failed=watcher->start("uvco-missing-parent/no-such-file");
    CHECK(!failed); CHECK(watcher->isActive()); CHECK(watcher->queued()==0);
    CHECK(watcher->stop()); CHECK(!watcher->isActive());
}
UVCO_TEST_MAIN
