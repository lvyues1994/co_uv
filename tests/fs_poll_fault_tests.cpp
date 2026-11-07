#include "fs_watcher_test.hpp"
using namespace watcher_test;
namespace fs = uvco::fs;
namespace {
uv_fs_poll_t* target = nullptr;
uv_fs_poll_cb callback = nullptr;
int initStatus = 0;
int startStatus = 0;
void inject(int status, std::uint64_t size = 0) {
    uv_stat_t prev{}, curr{};
    prev.st_size = size;
    curr.st_size = size + 1;
    CHECK(target && callback);
    callback(target, status, status < 0 ? nullptr : &prev, status < 0 ? nullptr : &curr);
    prev.st_size = curr.st_size = 9999; // callback must copy stack/native snapshots
}
int handles(uvco::Loop& loop) {
    int n = 0;
    uv_walk(loop.raw(), [](uv_handle_t*, void* p) { ++*static_cast<int*>(p); }, &n);
    return n;
}
}
extern "C" int __real_uv_fs_poll_init(uv_loop_t*, uv_fs_poll_t*);
extern "C" int __wrap_uv_fs_poll_init(uv_loop_t* loop, uv_fs_poll_t* handle) {
    return initStatus ? initStatus : __real_uv_fs_poll_init(loop, handle);
}
extern "C" int __real_uv_fs_poll_start(uv_fs_poll_t*, uv_fs_poll_cb, char const*, unsigned int);
extern "C" int __wrap_uv_fs_poll_start(uv_fs_poll_t* handle, uv_fs_poll_cb cb,
                                        char const* path, unsigned int interval) {
    target = handle;
    callback = cb;
    return startStatus ? startStatus : __real_uv_fs_poll_start(handle, cb, path, interval);
}

TEST_CASE("FsPoll queue owns snapshots and drops oldest past 32 without merging transitions") {
    uvco::Loop loop;
    Files files;
    files.write("file", "baseline");
    auto watcher = fs::FsPoll::start(loop, files.file("file"), 1s);
    CHECK(watcher);
    for (std::uint64_t i = 0; i < 40; ++i) inject(0, i);
    CHECK(watcher->queued() == 32 && watcher->overflowCount() == 8);
    CHECK(watcher->coalescedCount() == 0);
    for (std::uint64_t i = 8; i < 40; ++i) {
        auto r = uvco::run(loop, next(loop, *watcher));
        CHECK(r && r->prev.st_size == i && r->curr.st_size == i + 1);
    }
    inject(UV_ENOENT);
    inject(UV_ENOENT);
    CHECK(watcher->coalescedCount() == 1);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_ENOENT));
    inject(0, 50);
    co2::stop_source stop;
    stop.request_stop();
    r = uvco::run(loop, next(loop, *watcher), stop.get_token());
    CHECK(!r && r.error() == uvco::Errc::canceled && watcher->queued() == 1);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->curr.st_size == 51);
    inject(0, 52);
    CHECK(watcher->stop());
    CHECK(watcher->queued() == 0 && watcher->overflowCount() == 8);
    CHECK(watcher->setInterval(20ms));
    CHECK(watcher->queued() == 0 && watcher->overflowCount() == 0 && watcher->coalescedCount() == 0);
}

TEST_CASE("FsPoll failed init and start clean up and failed restart preserves old queue") {
    uvco::Loop loop;
    Files files;
    files.write("file", "baseline");
    int const before = handles(loop);
    initStatus = UV_ENOSYS;
    auto badInit = fs::FsPoll::start(loop, files.file("file"), 10ms);
    initStatus = 0;
    CHECK(!badInit && badInit.error() == static_cast<uvco::Errc>(UV_ENOSYS));
    CHECK(handles(loop) == before);
    startStatus = UV_ENOMEM;
    auto badStart = fs::FsPoll::start(loop, files.file("file"), 10ms);
    startStatus = 0;
    CHECK(!badStart && badStart.error() == uvco::Errc::no_memory);
    loop.runUntilIdle();
    CHECK(handles(loop) == before);
    auto watcher = fs::FsPoll::start(loop, files.file("file"), 10ms);
    CHECK(watcher);
    auto* originalTarget = target;
    auto originalCallback = callback;
    inject(0, 42);
    startStatus = UV_ENOMEM;
    auto failed = watcher->setInterval(20ms);
    startStatus = 0;
    target = originalTarget;
    callback = originalCallback;
    CHECK(!failed && watcher->isActive() && watcher->interval() == 10ms);
    CHECK(watcher->queued() == 1);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->curr.st_size == 43);
}

TEST_CASE("FsPoll callback completion wins late cancel and a waiting callback copies results") {
    uvco::Loop loop;
    Files files;
    files.write("file", "baseline");
    auto watcher = fs::FsPoll::start(loop, files.file("file"), 10ms);
    CHECK(watcher);
    auto pending = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!pending.isReady());
    pending.requestStop();
    inject(0, 80);
    CHECK(pending.isReady());
    auto r = pending.join();
    CHECK(r && r->prev.st_size == 80 && r->curr.st_size == 81);
}
UVCO_TEST_MAIN
