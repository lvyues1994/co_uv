#include "fs_watcher_test.hpp"
#include <new>
#include <stdexcept>
using namespace watcher_test;
namespace fs = uvco::fs;
namespace {
int initStatus = 0;
int startStatus = 0;
int appendFault = 0;
uv_fs_event_t* target = nullptr;
uv_fs_event_cb callback = nullptr;
void inject(char const* name, int events = UV_CHANGE, int status = 0) {
    CHECK(target && callback);
    callback(target, name, events, status);
}
int handles(uvco::Loop& loop) {
    int count = 0;
    uv_walk(loop.raw(), [](uv_handle_t*, void* n) { ++*static_cast<int*>(n); }, &count);
    return count;
}
}
extern "C" int __real_uv_fs_event_init(uv_loop_t*, uv_fs_event_t*);
extern "C" int __wrap_uv_fs_event_init(uv_loop_t* loop, uv_fs_event_t* handle) {
    return initStatus ? initStatus : __real_uv_fs_event_init(loop, handle);
}
extern "C" int __real_uv_fs_event_start(uv_fs_event_t*, uv_fs_event_cb, char const*, unsigned int);
extern "C" int __wrap_uv_fs_event_start(uv_fs_event_t* handle, uv_fs_event_cb cb,
                                         char const* path, unsigned int flags) {
    target = handle;
    callback = cb;
    return startStatus ? startStatus : __real_uv_fs_event_start(handle, cb, path, flags);
}
#if defined(UVCO_TEST_APPEND_SYMBOL)
std::string& realAppend(std::string*, char const*, std::size_t) asm("__real_" UVCO_TEST_APPEND_SYMBOL);
std::string& wrappedAppend(std::string*, char const*, std::size_t) asm("__wrap_" UVCO_TEST_APPEND_SYMBOL);
std::string& wrappedAppend(std::string* self, char const* data, std::size_t size) {
    int const fault = std::exchange(appendFault, 0);
    if (fault == 1) throw std::bad_alloc{};
    if (fault == 2) throw std::runtime_error{"event filename"};
    return realAppend(self, data, size);
}
#endif

TEST_CASE("FsEvent bounded queue coalesces adjacent names and drops oldest with counters") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsEvent::watch(loop, files.path);
    CHECK(watcher);
    inject("same", UV_RENAME);
    inject("same", UV_CHANGE);
    CHECK(watcher->queued() == 1 && watcher->coalescedCount() == 1);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->filename == "same" && r->events == (UV_RENAME | UV_CHANGE));
    for (int i = 0; i < 40; ++i) inject(("f" + std::to_string(i)).c_str());
    CHECK(watcher->queued() == fs::FsEvent::bufferCapacity());
    CHECK(watcher->overflowCount() == 8);
    for (int i = 8; i < 40; ++i) {
        r = uvco::run(loop, next(loop, *watcher));
        CHECK(r && r->filename == "f" + std::to_string(i));
    }
    CHECK(watcher->queued() == 0);
    inject(nullptr);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->filename.empty());
    inject(nullptr, 0, UV_EIO);
    inject(nullptr, 0, UV_EIO);
    CHECK(watcher->queued() == 1);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_EIO));
}

TEST_CASE("FsEvent pre-cancel preserves buffered data and first loop completion wins late cancel") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsEvent::watch(loop, files.path);
    CHECK(watcher);
    inject("retained");
    co2::stop_source stop;
    stop.request_stop();
    auto r = uvco::run(loop, next(loop, *watcher), stop.get_token());
    CHECK(!r && r.error() == uvco::Errc::canceled && watcher->queued() == 1);
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->filename == "retained");
    auto pending = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    CHECK(!pending.isReady());
    pending.requestStop();
    inject("completed-before-cancel-dispatch");
    CHECK(pending.isReady());
    r = pending.join();
    CHECK(r && r->filename == "completed-before-cancel-dispatch");
    auto closed = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    static_cast<void>(watcher->close());
    CHECK(!closed.isReady());
    loop.runUntilIdle();
    CHECK(closed.isReady());
    r = closed.join();
    CHECK(!r && r.error() == uvco::Errc::aborted);
}

TEST_CASE("FsEvent preserves a terminal backend status even if native handle was deactivated first") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsEvent::watch(loop, files.path);
    CHECK(watcher);
    CHECK(uv_fs_event_stop(target) == 0); // emulate backend deactivation before last error callback
    inject(nullptr, 0, UV_EIO);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == static_cast<uvco::Errc>(UV_EIO));
    CHECK(!watcher->isActive());
}

TEST_CASE("FsEvent unsupported initialization and failed start never close an uninitialized handle") {
    uvco::Loop loop;
    Files files;
    int const before = handles(loop);
    initStatus = UV_ENOSYS;
    auto unsupported = fs::FsEvent::watch(loop, files.path);
    initStatus = 0;
    CHECK(!unsupported && unsupported.error() == static_cast<uvco::Errc>(UV_ENOSYS));
    CHECK(handles(loop) == before);
    startStatus = UV_EACCES;
    auto denied = fs::FsEvent::watch(loop, files.path);
    startStatus = 0;
    CHECK(!denied && denied.error() == static_cast<uvco::Errc>(UV_EACCES));
    loop.runUntilIdle();
    CHECK(handles(loop) == before);
    auto watcher = fs::FsEvent::watch(loop, files.path);
    CHECK(watcher);
    auto* originalTarget = target;
    auto originalCallback = callback;
    inject("retained-on-failure");
    startStatus = UV_EACCES;
    auto failed = watcher->watch(files.path);
    startStatus = 0;
    target = originalTarget;
    callback = originalCallback;
    CHECK(!failed && watcher->isActive() && watcher->queued() == 1);
    auto retained = uvco::run(loop, next(loop, *watcher));
    CHECK(retained && retained->filename == "retained-on-failure");
    inject("usable");
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->filename == "usable");
}
#if defined(UVCO_TEST_APPEND_SYMBOL)
TEST_CASE("FsEvent filename allocation failure maps ENOMEM and other projection exceptions rethrow after cleanup") {
    uvco::Loop loop;
    Files files;
    auto watcher = fs::FsEvent::watch(loop, files.path);
    CHECK(watcher);
    appendFault = 1;
    inject("allocation");
    CHECK(appendFault == 0);
    auto r = uvco::run(loop, next(loop, *watcher));
    CHECK(!r && r.error() == uvco::Errc::no_memory);
    appendFault = 2;
    inject("exception");
    CHECK(appendFault == 0);
    bool caught = false;
    try { static_cast<void>(uvco::run(loop, next(loop, *watcher))); }
    catch (std::runtime_error const& e) { caught = std::string{e.what()} == "event filename"; }
    CHECK(caught);
    inject("after-exception");
    r = uvco::run(loop, next(loop, *watcher));
    CHECK(r && r->filename == "after-exception");
    auto pending = co2::spawn(loop, rawNext(*watcher));
    pump(loop);
    appendFault = 2;
    inject("pending-exception");
    CHECK(pending.isReady());
    caught = false;
    try { static_cast<void>(pending.join()); }
    catch (std::runtime_error const&) { caught = true; }
    CHECK(caught);
}
#endif
UVCO_TEST_MAIN
