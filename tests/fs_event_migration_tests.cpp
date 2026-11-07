#include <cstdio>
#include <fstream>
#include "test.hpp"
#include "uvco/fs/fs_event.hpp"
namespace {
// Watch a file created in the test working directory: file.path may be a
// path relative to the compiler's directory (e.g. CI presets), not ctest's.
struct WatchedFile {
    const char *path = "uvco-fs-event-migration.watch";
    WatchedFile() {
        std::ofstream(path) << "x";
    }
    ~WatchedFile() {
        std::remove(path);
    }
};
}
TEST_CASE("FsEvent start alias preserves transactional restart and counters") {
    WatchedFile file;
    uvco::Loop loop;
    auto watcher = uvco::fs::FsEvent::watch(loop, file.path);
    CHECK(watcher);
    CHECK(watcher->start(file.path));
    CHECK(watcher->isActive());
    auto failed = watcher->start("uvco-missing-parent/no-such-file");
    CHECK(!failed);
    CHECK(watcher->isActive());
    CHECK(watcher->queued() == 0);
    CHECK(watcher->stop());
    CHECK(!watcher->isActive());
}
UVCO_TEST_MAIN
