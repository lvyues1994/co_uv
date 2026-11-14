#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <co2/co2.hpp>

#include "test.hpp"
#ifdef UVCO_TEST_WRAP_NEW
#include "allocation_counter.hpp"
#endif
#include "uvco/core.hpp"

#ifndef UVCO_TEST_SOURCE_FILE
#error "UVCO_TEST_SOURCE_FILE must name an existing file"
#endif

namespace {

// 测试用的最小 fs_stat 封装：正式的 fs 模块在 v0.1 第 6 步实现。
struct FsStat : uvco::detail::ReqAwaiter<FsStat, uv_fs_t, uv_stat_t> {
    FsStat(uvco::Loop& loop, std::string path_) : ReqAwaiter{loop}, path{std::move(path_)} {}
    FsStat(FsStat&&) = default;
    ~FsStat() { uv_fs_req_cleanup(&req_); }

    int start(uv_loop_t* const loop) {
        startedAt = this;
        ++starts;
        if (onStart) onStart();
        return uv_fs_stat(loop, &req_, path.c_str(), &FsStat::onDone);
    }

    static void onDone(uv_fs_t* const req) { complete(req, static_cast<int>(req->result)); }

    uvco::Result<uv_stat_t> finish(int const status) {
        uv_stat_t const stat = req_.statbuf;
        uv_fs_req_cleanup(&req_);
        if (status < 0) return static_cast<uvco::Errc>(status);
        return stat;
    }

    void cancel() noexcept {
        cancelOnLoopThread = loop().isLoopThread();
        ++cancels;
        ReqAwaiter::cancel();
    }

    std::string path;
    void* startedAt = nullptr;
    int starts = 0;
    int cancels = 0;
    bool cancelOnLoopThread = false;
    void (*onStart)() = nullptr;
};

static_assert(std::is_same<co2::detail::AwaiterOf<FsStat&>,
                           co2::detail::AwaiterRef<FsStat>>::value,
              "an lvalue ReqAwaiter is awaited in place through co2::detail::AwaiterRef");
static_assert(sizeof(co2::detail::AwaiterOf<FsStat&>) == sizeof(void*), "");
static_assert(std::is_same<co2::detail::AwaiterOf<FsStat>, FsStat>::value,
              "an rvalue ReqAwaiter is still moved into the await slot");

// 一个不可取消的请求（uv_queue_work 的 after 回调签名不同）。
struct Work : uvco::detail::ReqAwaiter<Work, uv_work_t, int> {
    static constexpr bool cancellable = false;

    Work(uvco::Loop& loop, int (*fn_)()) : ReqAwaiter{loop}, fn{fn_} {}
    Work(Work&&) = default;

    int start(uv_loop_t* const loop) {
        return uv_queue_work(loop, &req_, &Work::onWork, &Work::onAfter);
    }
    static void onWork(uv_work_t* const req) {
        auto* const self = static_cast<Work*>(static_cast<ReqAwaiter*>(req->data));
        self->value = self->fn();
    }
    static void onAfter(uv_work_t* const req, int const status) { complete(req, status); }

    uvco::Result<int> finish(int const status) {
        if (status < 0) return static_cast<uvco::Errc>(status);
        return value;
    }

    int (*fn)();
    int value = 0;
};

auto statRvalue(uvco::Loop& loop, std::string path)
    CO2_BEG(co2::Task<uvco::Result<uv_stat_t>>, (loop, path), uvco::Result<uv_stat_t> r;) {
    CO2_AWAIT_SET(r, FsStat(loop, path));
    CHECK(loop.isLoopThread());
    CO2_RETURN(r);
}
CO2_END

auto statFrameLocal(uvco::Loop& loop, std::string path, std::string missing)
    CO2_BEG(co2::Task<int>, (loop, path, missing), FsStat op{loop, path};
            uvco::Result<uv_stat_t> first; uvco::Result<uv_stat_t> second;) {
    CO2_AWAIT_SET(first, op);
    CHECK(first);
    op.path = missing;
    CO2_AWAIT_SET(second, op);
    CHECK(not second && second.error() == uvco::Errc::no_such_file);
    CO2_RETURN(op.starts);
}
CO2_END


auto statFrameLocalIdentity(uvco::Loop& loop, std::string path)
    CO2_BEG(co2::Task<int>, (loop, path), FsStat op{loop, path}; void* frameAddr = nullptr;
            uvco::Result<uv_stat_t> first; std::size_t allocations = 0;) {
    frameAddr = &op;
#ifdef UVCO_TEST_WRAP_NEW
    allocation_test::begin();
#endif
    CO2_AWAIT_SET(first, op);
#ifdef UVCO_TEST_WRAP_NEW
    allocations = allocation_test::end();
    CHECK(allocations == 0);
#else
    static_cast<void>(allocations);
#endif
    CHECK(first);
    CHECK(op.startedAt == frameAddr);
    CO2_RETURN(op.starts);
}
CO2_END

auto statCounted(uvco::Loop& loop, FsStat& op)
    CO2_BEG(co2::Task<uvco::Result<uv_stat_t>>, (loop, op), uvco::Result<uv_stat_t> r;) {
    CO2_AWAIT_SET(r, op);
    CHECK(loop.isLoopThread());
    CO2_RETURN(r);
}
CO2_END

auto work(uvco::Loop& loop, int (*fn)())
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, fn), uvco::Result<int> r;) {
    CO2_AWAIT_SET(r, Work(loop, fn));
    CO2_RETURN(r);
}
CO2_END

// ---- 在途取消：线程池只有 1 个线程，被 blocker 占住，fs 请求排在队列里 ----

std::promise<void>* gStatStarted = nullptr;
std::promise<void>* gReleaseBlocker = nullptr;
std::shared_future<void> gBlockerGate;

void signalStarted() { gStatStarted->set_value(); }

auto statThenRelease(uvco::Loop& loop, FsStat& op)
    CO2_BEG(co2::Task<uvco::Result<uv_stat_t>>, (loop, op), uvco::Result<uv_stat_t> r;) {
    CO2_AWAIT_SET(r, op);
    gReleaseBlocker->set_value();
    CO2_RETURN(r);
}
CO2_END

} // namespace

TEST_CASE("ReqAwaiter: uv_fs_stat on an existing file (rvalue awaiter)") {
    uvco::Loop loop;
    auto const r = uvco::run(loop, statRvalue(loop, UVCO_TEST_SOURCE_FILE));
    CHECK(r);
    CHECK(r->st_size > 0U);
}

TEST_CASE("ReqAwaiter: error is returned as Result") {
    uvco::Loop loop;
    auto const r = uvco::run(loop, statRvalue(loop, "/definitely/not/here/uvco"));
    CHECK(not r);
    CHECK(r.error() == uvco::Errc::no_such_file);
    CHECK(r.errorCode() == std::error_code{uvco::Errc::no_such_file});
}

TEST_CASE("ReqAwaiter: frame-local awaiter is reusable") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, statFrameLocal(loop, UVCO_TEST_SOURCE_FILE, "/no/such/uvco")) == 2);
}


TEST_CASE("ReqAwaiter: lvalue await uses the frame-local object and does not allocate") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, statFrameLocalIdentity(loop, UVCO_TEST_SOURCE_FILE)) == 1);
}

TEST_CASE("ReqAwaiter: stop requested before await completes with ECANCELED, no start") {
    uvco::Loop loop;
    FsStat op{loop, UVCO_TEST_SOURCE_FILE};
    co2::stop_source source;
    source.request_stop();
    auto const r = uvco::run(loop, statCounted(loop, op), source.get_token());
    CHECK(not r && r.error() == uvco::Errc::canceled);
    CHECK(op.starts == 0);
}

TEST_CASE("ReqAwaiter: possible-but-unrequested stop leaves the request alone") {
    uvco::Loop loop;
    FsStat op{loop, UVCO_TEST_SOURCE_FILE};
    co2::stop_source source;
    auto const r = uvco::run(loop, statCounted(loop, op), source.get_token());
    CHECK(r);
    CHECK(op.cancels == 0);
    // 完成后再请求停止：stop_callback 已注销，不会碰到已销毁/空闲的 awaiter。
    source.request_stop();
    CHECK(op.cancels == 0);
}

TEST_CASE("ReqAwaiter: cross-thread stop cancels an in-flight request on the loop thread") {
    uvco::Loop loop;

    std::promise<void> started;
    std::promise<void> release;
    gStatStarted = &started;
    gReleaseBlocker = &release;
    gBlockerGate = release.get_future().share();

    // 占住唯一的线程池线程。
    uv_work_t blocker{};
    bool blockerDone = false;
    blocker.data = &blockerDone;
    CHECK(uv_queue_work(
              loop.raw(), &blocker, [](uv_work_t*) { gBlockerGate.wait(); },
              [](uv_work_t* const req, int) { *static_cast<bool*>(req->data) = true; }) == 0);

    FsStat op{loop, UVCO_TEST_SOURCE_FILE};
    op.onStart = &signalStarted;
    co2::stop_source source;
    auto startedFuture = started.get_future();
    std::thread stopper{[&] {
        startedFuture.wait();
        source.request_stop(); // stop_callback 在本线程执行：只 post 到 loop
    }};

    auto const r = uvco::run(loop, statThenRelease(loop, op), source.get_token());
    stopper.join();
    CHECK(not r && r.error() == uvco::Errc::canceled);
    CHECK(op.cancels == 1);
    CHECK(op.cancelOnLoopThread);

    // 让 blocker 的 after 回调跑完，Loop 才能无请求地关闭。
    uv_run(loop.raw(), UV_RUN_DEFAULT);
    CHECK(blockerDone);
}

TEST_CASE("ReqAwaiter: non-cancellable request (uv_queue_work)") {
    uvco::Loop loop;
    co2::stop_source source;
    auto const r = uvco::run(loop, work(loop, [] { return 99; }), source.get_token());
    CHECK(r && *r == 99);
}

int main() {
    // 必须在 libuv 首次使用前设置：在途取消用例依赖单线程池，且 fs 请求必须走线程池
    // （Linux 上 libuv 可能把 fs 请求交给 io_uring，那样 uv_cancel 只会返回 EBUSY）。
    setenv("UV_THREADPOOL_SIZE", "1", 1);
    setenv("UV_USE_IO_URING", "0", 1);
    return ::uvco_test::runAll();
}
