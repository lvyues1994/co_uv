// Loop 生命周期：保活（KeepAlive / drain）、析构兜底关闭句柄、SIGPIPE（docs/architecture.md §4.1、§4.2）。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <csignal>
#endif

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/uvco.hpp"

using namespace std::chrono_literals;
using uvco::Errc;
using uvco::Result;

namespace {

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point const start) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

auto tickResult(uvco::Timer& timer)
    CO2_BEG(co2::Task<Result<std::uint64_t>>, (timer), Result<std::uint64_t> r;) {
    CO2_AWAIT_SET(r, timer.tick());
    CO2_RETURN(r);
}
CO2_END

auto sleepFor(uvco::Loop& loop, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<int>, (loop, d), Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CO2_RETURN(r ? 1 : 0);
}
CO2_END

// tick 被 ~Loop 中止后，在 ~Loop 的驱动循环里再开一个新计时器并等待：
// 析构必须把它也关掉（下一轮 closeResidents），而不是卡住或 uv_loop_close 失败。
auto reopenDuringTeardown(uvco::Loop& loop, uvco::Timer& first, int& stage)
    CO2_BEG(co2::Task<int>, (loop, first, stage), Result<std::uint64_t> r;
            std::unique_ptr<uvco::Timer> second;) {
    CO2_AWAIT_SET(r, first.tick());
    CHECK(not r && r.error() == Errc::aborted);
    stage = 1;
    second.reset(new uvco::Timer{loop});
    CHECK(second->start(10s));
    CO2_AWAIT_SET(r, second->tick());
    CHECK(not r && r.error() == Errc::aborted);
    stage = 2;
    CO2_RETURN(stage);
}
CO2_END

auto acceptResult(uvco::Tcp& server) CO2_BEG(co2::Task<Errc>, (server), Result<uvco::Tcp> c;) {
    CO2_AWAIT_SET(c, server.accept());
    CO2_RETURN(c ? Errc{} : c.error());
}
CO2_END

struct FlagCallback : uvco::detail::LoopCallback {
    std::atomic<bool> ran{false};
    std::atomic<bool> onLoopThread{false};
    uvco::Loop* loop = nullptr;
    FlagCallback() {
        invoke = [](uvco::detail::LoopCallback* const self) noexcept {
            auto* const flag = static_cast<FlagCallback*>(self);
            flag->onLoopThread = flag->loop->isLoopThread();
            flag->ran = true;
        };
    }
};

} // namespace

// ---------------------------------------------------------------- 保活

TEST_CASE("drain() on an idle loop returns immediately") {
    uvco::Loop loop;
    auto const start = Clock::now();
    uvco::drain(loop);
    CHECK(msSince(start) < 1000);
}

TEST_CASE("drain() runs a spawned task to completion") {
    uvco::Loop loop;
    auto handle = co2::spawn(loop, sleepFor(loop, 10ms));
    uvco::drain(loop);
    CHECK(handle.isReady());
    CHECK(handle.join() == 1);
}

TEST_CASE("KeepAlive released on another thread keeps drain() waiting for the post") {
    for (int round = 0; round < 10; ++round) {
        uvco::Loop loop;
        FlagCallback callback;
        callback.loop = &loop;
        uvco::Loop::KeepAlive hold = loop.keepAlive();
        auto const start = Clock::now();
        std::thread worker{[&loop, &callback, h = std::move(hold), round]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds{round % 3 == 0 ? 20 : round % 3});
            loop.post(&callback);
            h.reset(); // 任意线程释放
        }};
        uvco::drain(loop);
        worker.join();
        CHECK(callback.ran);
        CHECK(callback.onLoopThread);
        if (round % 3 == 0) CHECK(msSince(start) >= 15);
    }
}

TEST_CASE("KeepAlive is move-only and reset() releases it") {
    uvco::Loop loop;
    uvco::Loop::KeepAlive a = loop.keepAlive();
    CHECK(static_cast<bool>(a));
    uvco::Loop::KeepAlive b = std::move(a);
    CHECK(not a);
    CHECK(static_cast<bool>(b));
    uvco::Loop::KeepAlive c = loop.keepAlive();
    c = std::move(b); // 释放 c 原来的一份
    CHECK(not b);
    c.reset();
    CHECK(not c);
    auto const start = Clock::now();
    uvco::drain(loop); // 没有存活的保活：立即返回
    CHECK(msSince(start) < 1000);
}

TEST_CASE("run() does not wait for unrelated active handles") {
    uvco::Loop loop;
    uvco::Timer timer{loop};
    CHECK(timer.start(10s));
    auto const start = Clock::now();
    CHECK(uvco::run(loop, sleepFor(loop, 1ms)) == 1);
    CHECK(msSince(start) < 5000);
}

// ---------------------------------------------------------------- 析构兜底

TEST_CASE("~Loop closes a handle that outlives it; the handle becomes an orphan") {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    uvco::Timer kept{*loop};
    uvco::Timer closedLater{*loop};
    CHECK(kept.start(10s));
    CHECK(closedLater.start(10s));
    loop.reset();
    CHECK(uvco::Loop::current() == nullptr);
    CHECK(kept.isOpen()); // 仍持有（孤儿）控制块
    static_cast<void>(closedLater.close()); // 孤儿的 close()：直接释放
    CHECK(not closedLater.isOpen());
    // kept 在这里析构：直接释放，不触碰已销毁的 Loop
}

TEST_CASE("~Loop ends a pending tick with aborted") {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    uvco::Timer timer{*loop};
    CHECK(timer.start(10s));
    auto handle = co2::spawn(*loop, tickResult(timer));
    static_cast<void>(uv_run(loop->raw(), UV_RUN_NOWAIT)); // 启动 Task，挂起在 tick()
    CHECK(not handle.isReady());
    loop.reset();
    CHECK(handle.isReady());
    Result<std::uint64_t> const r = handle.join();
    CHECK(not r && r.error() == Errc::aborted);
}

TEST_CASE("~Loop also closes handles opened by coroutines it resumes") {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    uvco::Timer timer{*loop};
    CHECK(timer.start(10s));
    int stage = 0;
    auto handle = co2::spawn(*loop, reopenDuringTeardown(*loop, timer, stage));
    static_cast<void>(uv_run(loop->raw(), UV_RUN_NOWAIT));
    CHECK(stage == 0);
    loop.reset();
    CHECK(stage == 2);
    CHECK(handle.join() == 2);
}

TEST_CASE("~Loop aborts a pending accept and closes an open listener") {
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    uvco::Tcp server{*loop};
    CHECK(server.bind(*uvco::SocketAddress::parse("127.0.0.1", 0)));
    CHECK(server.listen());
    auto handle = co2::spawn(*loop, acceptResult(server));
    static_cast<void>(uv_run(loop->raw(), UV_RUN_NOWAIT));
    loop.reset();
    CHECK(handle.isReady());
    CHECK(handle.join() == Errc::aborted);
    CHECK(server.isOpen());
}

TEST_CASE("~Loop closes foreign raw handles and nulls their data") {
    uv_timer_t raw;
    int marker = 0;
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    CHECK(uv_timer_init(loop->raw(), &raw) == 0);
    raw.data = &marker;
    CHECK(uv_timer_start(&raw, [](uv_timer_t*) {}, 10000, 0) == 0);
    loop.reset(); // 以前：uv_loop_close 返回 EBUSY → 契约违规
    CHECK(raw.data == nullptr);
}

TEST_CASE("~Loop right after closing a handle and finishing a run()") {
    uvco::Loop loop;
    uvco::Timer timer{loop};
    CHECK(timer.start(1ms, 1ms));
    CHECK(uvco::run(loop, tickResult(timer)));
    static_cast<void>(timer.close());
}

#ifndef _WIN32
TEST_CASE("constructing a Loop ignores SIGPIPE") {
    uvco::Loop loop;
    struct sigaction action {};
    CHECK(sigaction(SIGPIPE, nullptr, &action) == 0);
    CHECK(action.sa_handler == SIG_IGN);
}
#endif

UVCO_TEST_MAIN
