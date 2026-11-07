#include "fs_watcher_test.hpp"
#include <thread>
#include <type_traits>
using namespace watcher_test;
namespace fs = uvco::fs;

namespace {
auto matching(uvco::Loop& loop, fs::FsEvent& watcher, std::string name, int mask)
    CO2_BEG(co2::Task<bool>, (loop, watcher, name, mask), uvco::Result<fs::FsEventInfo> result;) {
    for (;;) {
        CO2_AWAIT_SET(result, uvco::timeout(loop, 5s, watcher.next()));
        if (!result) CO2_RETURN(false);
        if ((result->filename == name ||
             (name == "renamed" && result->filename == "rename-source")) &&
            (result->events & mask) != 0) CO2_RETURN(true);
    }
} CO2_END
}

TEST_CASE("FsEvent detects directory create modify rename delete with owned filenames") {
    uvco::Loop loop;
    Files files;
    files.write("modified", "x");
    files.write("rename-source", "x");
    files.write("deleted", "x");
    auto watcher = fs::FsEvent::watch(loop, files.path);
    if (!watcher && watcher.error() == static_cast<uvco::Errc>(UV_ENOSYS)) return;
    CHECK(watcher);
    files.write("created", "new");
    CHECK(uvco::run(loop, matching(loop, *watcher, "created", UV_RENAME)));
    files.write("modified", "more", true);
    CHECK(uvco::run(loop, matching(loop, *watcher, "modified", UV_CHANGE)));
    files.rename("rename-source", "renamed");
    CHECK(uvco::run(loop, matching(loop, *watcher, "renamed", UV_RENAME)));
    files.remove("deleted");
    CHECK(uvco::run(loop, matching(loop, *watcher, "deleted", UV_RENAME)));
}

TEST_CASE("FsEvent cancellation leaves watch running and explicit stop aborts later") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsEvent::watch(loop, files.path);
    if (!watcher && watcher.error() == static_cast<uvco::Errc>(UV_ENOSYS)) return;
    CHECK(watcher);
    co2::stop_source stop;
    stop.request_stop();
    auto canceled = uvco::run(loop, next(loop, *watcher), stop.get_token());
    CHECK(!canceled && canceled.error() == uvco::Errc::canceled);
    auto pending = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!pending.isReady());
    std::thread cancel{[&pending] { pending.requestStop(); }};
    cancel.join();
    while (!pending.isReady()) pump(loop);
    auto result = pending.join();
    CHECK(!result && result.error() == uvco::Errc::canceled);
    CHECK(watcher->isActive());
    files.write("after-cancel", "x");
    CHECK(uvco::run(loop, matching(loop, *watcher, "after-cancel", UV_RENAME)));
    // Restart clears any remaining notifications, then stop aborts only on loop dispatch.
    CHECK(watcher->watch(files.path));
    auto stopped = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!stopped.isReady());
    CHECK(watcher->stop());
    CHECK(!stopped.isReady());
    loop.runUntilIdle();
    CHECK(stopped.isReady());
    result = stopped.join();
    CHECK(!result && result.error() == uvco::Errc::aborted);
    result = uvco::run(loop, next(loop, *watcher));
    CHECK(!result && result.error() == uvco::Errc::invalid_argument);
}

TEST_CASE("FsEvent close leases and Loop teardown retain blocks safely") {
    Files files;
    {
        uvco::Loop loop;
        auto watcher = fs::FsEvent::watch(loop, files.path);
        if (!watcher && watcher.error() == static_cast<uvco::Errc>(UV_ENOSYS)) return;
        CHECK(watcher);
        auto lease = watcher->next();
        auto moved = std::move(*watcher);
        static_cast<void>(moved.close());
        loop.runUntilIdle();
        auto r = uvco::run(loop, held<decltype(lease), fs::FsEventInfo>(std::move(lease)));
        CHECK(!r && r.error() == uvco::Errc::aborted);
    }
    auto loop = std::unique_ptr<uvco::Loop>{new uvco::Loop};
    auto watcher = fs::FsEvent::watch(*loop, files.path);
    CHECK(watcher);
    auto pending = co2::spawn(*loop, rawNext(*watcher));
    pump(*loop);
    CHECK(!pending.isReady());
    loop.reset();
    CHECK(pending.isReady());
    auto r = pending.join();
    CHECK(!r && r.error() == uvco::Errc::aborted);
    static_cast<void>(watcher->close());
}

TEST_CASE("FsEvent unsupported and invalid flags are explicit errors") {
    uvco::Loop loop;
    Files files;
    auto r = fs::FsEvent::watch(loop, files.path, UV_FS_EVENT_STAT);
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOTSUP));
    r = fs::FsEvent::watch(loop, files.path, UV_FS_EVENT_WATCH_ENTRY);
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOTSUP));
    r = fs::FsEvent::watch(loop, files.path, 0x80U);
    CHECK(!r && r.error() == uvco::Errc::invalid_argument);
#if !defined(_WIN32) && !defined(__APPLE__)
    r = fs::FsEvent::watch(loop, files.path, UV_FS_EVENT_RECURSIVE);
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOTSUP));
#endif
    r = fs::FsEvent::watch(loop, std::string{"a\0b", 3});
    CHECK(!r && r.error() == uvco::Errc::invalid_argument);
    static_assert(!std::is_copy_constructible<fs::FsEvent>::value, "move-only watcher");
    static_assert(std::is_nothrow_move_constructible<fs::FsEvent>::value, "noexcept move");
}
#if defined(__linux__)
TEST_CASE("FsEvent repeated restarts and close do not leak native fds") {
    Files files;
    int const before = fdCount();
    for (int round = 0; round < 20; ++round) {
        uvco::Loop loop;
        auto watcher = fs::FsEvent::watch(loop, files.path);
        CHECK(watcher);
        for (int restart = 0; restart < 8; ++restart) CHECK(watcher->watch(files.path));
        CHECK(watcher->stop());
        static_cast<void>(watcher->close());
        loop.runUntilIdle();
    }
    CHECK(fdCount() == before);
}
#endif
UVCO_TEST_MAIN
