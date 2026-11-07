#include "fs_watcher_test.hpp"
#include <limits>
#include <thread>
#include <type_traits>
using namespace watcher_test;
namespace fs = uvco::fs;

TEST_CASE("FsPoll reports missing file then create modify delete and recovery snapshots") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsPoll::start(loop, files.file("file"), 10ms);
    CHECK(watcher);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOENT));
    CHECK(watcher->isActive());
    files.write("file", "x");
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->prev.st_size == 0 && r->curr.st_size == 1);
    files.write("file", "more", true);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->prev.st_size == 1 && r->curr.st_size == 5);
    files.remove("file");
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOENT));
    files.write("file", "recreated");
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->curr.st_size == 9);
}

TEST_CASE("FsPoll positive interval changes replace handles even with initial stat in flight") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsPoll::start(loop, files.file("missing"), 100ms);
    CHECK(watcher && watcher->interval() == 100ms);
    for (int i = 0; i < 40; ++i) CHECK(watcher->setInterval(5ms));
    CHECK(watcher->interval() == 5ms);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOENT));
    CHECK(!watcher->setInterval(0ms));
    CHECK(!watcher->setInterval(-1ms));
    auto const huge = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>((std::numeric_limits<unsigned int>::max)()) + 1};
    CHECK(!watcher->setInterval(huge));
    CHECK(watcher->isActive() && watcher->interval() == 5ms);
    CHECK(watcher->start(files.file("other-missing"), 20ms));
    CHECK(watcher->interval() == 20ms);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOENT));
    auto invalid = fs::FsPoll::start(loop, std::string{"a\0b", 3}, 1ms);
    CHECK(!invalid && invalid.error() == uvco::Errc::invalid_argument);
    static_assert(!std::is_copy_constructible<fs::FsPoll>::value, "move-only poll watcher");
}

namespace {
auto timeoutNext(uvco::Loop& loop, fs::FsPoll& watcher)
    CO2_BEG(co2::Task<uvco::Result<fs::FsPollChange>>, (loop, watcher),
            uvco::Result<fs::FsPollChange> result;) {
    CO2_AWAIT_SET(result, uvco::timeout(loop, 20ms, watcher.next()));
    CO2_RETURN(std::move(result));
} CO2_END
}

TEST_CASE("FsPoll cancel timeout stop restart and close preserve waiter lifetime and resume policy") {
    uvco::Loop loop;
    Files files;
    files.write("file", "baseline");
    auto watcher = fs::FsPoll::start(loop, files.file("file"), 10ms);
    CHECK(watcher);
    co2::stop_source stop;
    stop.request_stop();
    auto r = uvco::run(loop, next(loop, *watcher), stop.get_token());
    CHECK(!r && r.error() == uvco::Errc::canceled);
    r = uvco::run(loop, timeoutNext(loop, *watcher));
    CHECK(!r && r.error() == uvco::Errc::timed_out && watcher->isActive());
    auto pending = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!pending.isReady());
    std::thread cancel{[&pending] { pending.requestStop(); }};
    cancel.join();
    while (!pending.isReady()) pump(loop);
    r = pending.join();
    CHECK(!r && r.error() == uvco::Errc::canceled && watcher->isActive());
    auto restarted = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!restarted.isReady());
    CHECK(watcher->setInterval(20ms));
    CHECK(!restarted.isReady());
    while (!restarted.isReady()) pump(loop);
    r = restarted.join();
    CHECK(!r && r.error() == uvco::Errc::aborted);
    auto stopped = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(watcher->stop());
    CHECK(!stopped.isReady());
    loop.runUntilIdle();
    CHECK(stopped.isReady());
    r = stopped.join();
    CHECK(!r && r.error() == uvco::Errc::aborted);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == uvco::Errc::invalid_argument);
    CHECK(watcher->start(files.file("file"), 10ms));
    auto closed = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    static_cast<void>(watcher->close());
    CHECK(!closed.isReady());
    loop.runUntilIdle();
    CHECK(closed.isReady());
    r = closed.join();
    CHECK(!r && r.error() == uvco::Errc::aborted);
}

TEST_CASE("FsPoll next lease survives close and owner outlives Loop safely") {
    Files files;
    files.write("file", "baseline");
    {
        uvco::Loop loop;
        auto watcher = fs::FsPoll::start(loop, files.file("file"), 10ms);
        CHECK(watcher);
        auto lease = watcher->next();
        static_cast<void>(watcher->close());
        loop.runUntilIdle();
        auto r = uvco::run(loop, held<decltype(lease), fs::FsPollChange>(std::move(lease)));
        CHECK(!r && r.error() == uvco::Errc::aborted);
    }
    auto loop = std::unique_ptr<uvco::Loop>{new uvco::Loop};
    auto watcher = fs::FsPoll::start(*loop, files.file("file"), 10ms);
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

#if defined(__linux__)
TEST_CASE("FsPoll repeated in-flight restart and close does not leak fds") {
    Files files;
    int const before = fdCount();
    for (int round = 0; round < 20; ++round) {
        uvco::Loop loop;
        auto watcher = fs::FsPoll::start(loop, files.file("missing"), 10ms);
        CHECK(watcher);
        for (int restart = 0; restart < 8; ++restart) CHECK(watcher->setInterval(20ms));
        static_cast<void>(watcher->close());
        loop.runUntilIdle();
    }
    CHECK(fdCount() == before);
}
#endif
UVCO_TEST_MAIN
