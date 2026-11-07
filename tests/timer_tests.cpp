#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/uvco.hpp"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using uvco::Errc;

namespace {

long long msSince(Clock::time_point const start) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

// ---------------------------------------------------------------- sleep

auto sleepAndLog(uvco::Loop& loop, std::chrono::milliseconds d, std::vector<int>& log, int id)
    CO2_BEG(co2::Task<>, (loop, d, log, id), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CHECK(r);
    CHECK(loop.isLoopThread());
    log.push_back(id);
}
CO2_END

auto ordering(uvco::Loop& loop, std::vector<int>& log) CO2_BEG(co2::Task<>, (loop, log)) {
    CO2_AWAIT(co2::whenAll(sleepAndLog(loop, 30ms, log, 3), sleepAndLog(loop, 10ms, log, 1),
                           sleepAndLog(loop, 20ms, log, 2), sleepAndLog(loop, 0ms, log, 0)));
}
CO2_END

auto timedSleep(uvco::Loop& loop, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<long long>, (loop, d), Clock::time_point start; uvco::Result<void> r;) {
    start = Clock::now();
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CHECK(r);
    CO2_RETURN(msSince(start));
}
CO2_END

auto sleepResult(uvco::Loop& loop, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CO2_RETURN(r);
}
CO2_END

auto frameLocalSleep(uvco::Loop& loop)
    CO2_BEG(co2::Task<int>, (loop), uvco::Sleep nap{loop, 1ms}; uvco::Result<void> r; int n{};) {
    for (n = 0; n < 3; ++n) {
        CO2_AWAIT_SET(r, nap);
        CHECK(r);
    }
    nap.setDuration(0ms);
    CO2_AWAIT_SET(r, nap);
    CHECK(r);
    CO2_RETURN(n + 1);
}
CO2_END

auto stopAfter(uvco::Loop& loop, co2::stop_source& source, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<>, (loop, source, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CHECK(r);
    source.request_stop();
}
CO2_END

auto cancelSleepSameThread(uvco::Loop& loop, co2::stop_source& source)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, source),
            std::tuple<uvco::Result<void>> results;) {
    CO2_AWAIT_SET(results, co2::whenAll(sleepResult(loop, 10s), stopAfter(loop, source, 5ms)));
    CO2_RETURN(std::get<0>(results));
}
CO2_END

auto manySleeps(uvco::Loop& loop, int count)
    CO2_BEG(co2::Task<std::size_t>, (loop, count), std::vector<co2::Task<>> tasks;
            std::vector<int> log;) {
    for (int i = 0; i < count; ++i)
        tasks.push_back(sleepAndLog(loop, std::chrono::milliseconds{i % 5}, log, i));
    CO2_AWAIT(co2::whenAll(std::move(tasks)));
    CO2_RETURN(log.size());
}
CO2_END

// ---------------------------------------------------------------- Timer

auto tickResult(uvco::Timer& timer)
    CO2_BEG(co2::Task<uvco::Result<std::uint64_t>>, (timer), uvco::Result<std::uint64_t> r;) {
    CO2_AWAIT_SET(r, timer.tick());
    CO2_RETURN(r);
}
CO2_END

auto repeatedTicks(uvco::Loop& loop, int count)
    CO2_BEG(co2::Task<std::uint64_t>, (loop, count), uvco::Timer timer{loop};
            uvco::Result<std::uint64_t> r; std::uint64_t total{}; int i{};) {
    CHECK(timer.start(2ms, 2ms));
    CHECK(timer.isActive());
    CHECK(timer.repeat() == 2ms);
    for (i = 0; i < count; ++i) {
        CO2_AWAIT_SET(r, timer.tick());
        CHECK(r && *r >= 1U);
        CHECK(loop.isLoopThread());
        total += *r;
    }
    timer.stop();
    CHECK(not timer.isActive());
    CO2_AWAIT(timer.close());
    CHECK(not timer.isOpen());
    CO2_RETURN(total);
}
CO2_END

auto accumulates(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::uint64_t>, (loop), uvco::Timer timer{loop};
            uvco::Result<void> slept; uvco::Result<std::uint64_t> r;) {
    CHECK(timer.start(1ms, 1ms));
    CO2_AWAIT_SET(slept, uvco::sleep(loop, 30ms)); // 没人 tick：到期累计
    CHECK(slept);
    CO2_AWAIT_SET(r, timer.tick()); // 立即完成
    CHECK(r);
    CO2_RETURN(*r);
}
CO2_END

auto zeroDelayTimer(uvco::Loop& loop)
    CO2_BEG(co2::Task<uvco::Result<std::uint64_t>>, (loop), uvco::Timer timer{loop};
            uvco::Result<std::uint64_t> r;) {
    CHECK(timer.start(0ms));
    CO2_AWAIT_SET(r, timer.tick());
    CO2_RETURN(r);
}
CO2_END

auto notStarted(uvco::Loop& loop)
    CO2_BEG(co2::Task<int>, (loop), uvco::Timer timer{loop}; uvco::Result<std::uint64_t> r;) {
    CO2_AWAIT_SET(r, timer.tick());
    CHECK(not r && r.error() == Errc::invalid_argument);
    CHECK(timer.start(1ms)); // 一次性
    CO2_AWAIT_SET(r, timer.tick());
    CHECK(r && *r == 1U);
    CO2_AWAIT_SET(r, timer.tick()); // 已到期且被取走、不再重复
    CHECK(not r && r.error() == Errc::invalid_argument);
    CO2_RETURN(0);
}
CO2_END

auto stopTimerAfter(uvco::Loop& loop, uvco::Timer& timer)
    CO2_BEG(co2::Task<>, (loop, timer), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5ms));
    timer.stop();
}
CO2_END

auto stopWhileTicking(uvco::Loop& loop)
    CO2_BEG(co2::Task<uvco::Result<std::uint64_t>>, (loop), uvco::Timer timer{loop};
            std::tuple<uvco::Result<std::uint64_t>> results;) {
    CHECK(timer.start(10s));
    CO2_AWAIT_SET(results, co2::whenAll(tickResult(timer), stopTimerAfter(loop, timer)));
    CO2_RETURN(std::get<0>(results));
}
CO2_END

auto closeTimerAfter(uvco::Loop& loop, uvco::Timer& timer, bool& closed)
    CO2_BEG(co2::Task<>, (loop, timer, closed), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5ms));
    CO2_AWAIT(timer.close());
    closed = true;
    CHECK(not timer.isOpen());
    CO2_AWAIT(timer.close()); // 空句柄：立即完成
}
CO2_END

auto closeWhileTicking(uvco::Loop& loop, bool& closed)
    CO2_BEG(co2::Task<uvco::Result<std::uint64_t>>, (loop, closed), uvco::Timer timer{loop};
            std::tuple<uvco::Result<std::uint64_t>> results;) {
    CHECK(timer.start(10s, 10s));
    CO2_AWAIT_SET(results, co2::whenAll(tickResult(timer), closeTimerAfter(loop, timer, closed)));
    CO2_RETURN(std::get<0>(results));
}
CO2_END

auto cancelTickKeepsCount(uvco::Loop& loop, co2::stop_source& source)
    CO2_BEG(co2::Task<int>, (loop, source), uvco::Timer timer{loop};
            std::tuple<uvco::Result<std::uint64_t>> results; uvco::Result<void> slept;) {
    CHECK(timer.start(30ms, 30ms));
    CO2_AWAIT_SET(results, co2::whenAll(tickResult(timer), stopAfter(loop, source, 5ms)));
    CHECK(not std::get<0>(results) && std::get<0>(results).error() == Errc::canceled);
    CHECK(timer.isActive()); // 取消等待不会停止计时器
    CO2_RETURN(0);
}
CO2_END

auto movedTimer(uvco::Loop& loop)
    CO2_BEG(co2::Task<int>, (loop), uvco::Timer first{loop}; uvco::Timer second{loop};
            uvco::Result<std::uint64_t> r;) {
    CHECK(first.start(1ms, 1ms));
    second = std::move(first); // 旧 second 被关闭（无挂起操作）
    CHECK(not first.isOpen());
    CHECK(second.isOpen());
    CO2_AWAIT_SET(r, second.tick());
    CHECK(r && *r >= 1U);
    CO2_RETURN(0);
}
CO2_END

auto destroyRunningTimer(uvco::Loop& loop)
    CO2_BEG(co2::Task<int>, (loop), std::unique_ptr<uvco::Timer> timer; uvco::Result<void> r;) {
    timer.reset(new uvco::Timer{loop});
    CHECK(timer->start(1ms, 1ms));
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5ms));
    timer.reset(); // 无挂起操作：uv_close，控制块在 close 回调里释放
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5ms));
    CHECK(r);
    CO2_RETURN(0);
}
CO2_END

} // namespace

// ---------------------------------------------------------------- sleep cases

TEST_CASE("sleep: completions are ordered by deadline") {
    uvco::Loop loop;
    std::vector<int> log;
    uvco::run(loop, ordering(loop, log));
    CHECK((log == std::vector<int>{0, 1, 2, 3}));
}

TEST_CASE("sleep: waits at least the requested duration") {
    uvco::Loop loop;
    // libuv 的时钟是毫秒粒度、且以单调时钟截断：实测偶有 19ms（ASan 下更常见），留 2ms 余量。
    CHECK(uvco::run(loop, timedSleep(loop, 20ms)) >= 18);
}

TEST_CASE("sleep: zero delay completes") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, timedSleep(loop, 0ms)) < 1000);
}

TEST_CASE("sleep: frame-local Sleep is reusable") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, frameLocalSleep(loop)) == 4);
}

TEST_CASE("sleep: stop requested before waiting -> ECANCELED") {
    uvco::Loop loop;
    co2::stop_source source;
    source.request_stop();
    auto const start = Clock::now();
    auto const r = uvco::run(loop, sleepResult(loop, 10s), source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
    CHECK(msSince(start) < 1000);
}

TEST_CASE("sleep: stop during wait (same thread) -> ECANCELED") {
    uvco::Loop loop;
    co2::stop_source source;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, cancelSleepSameThread(loop, source), source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
    CHECK(msSince(start) < 1000);
}

TEST_CASE("sleep: stop during wait from another thread -> ECANCELED") {
    for (int round = 0; round < 20; ++round) {
        uvco::Loop loop;
        co2::stop_source source;
        std::thread stopper{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{round % 4});
            source.request_stop();
        }};
        auto const start = Clock::now();
        auto const r = uvco::run(loop, sleepResult(loop, 10s), source.get_token());
        stopper.join();
        CHECK(not r && r.error() == Errc::canceled);
        CHECK(msSince(start) < 1000);
    }
}

TEST_CASE("sleep: many concurrent sleeps") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, manySleeps(loop, 1000)) == 1000U);
}

// ---------------------------------------------------------------- Timer cases

TEST_CASE("Timer: repeated ticks, then close() awaited") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, repeatedTicks(loop, 10)) >= 10U);
}

TEST_CASE("Timer: expirations accumulate while nobody waits") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, accumulates(loop)) > 1U);
}

TEST_CASE("Timer: zero delay") {
    uvco::Loop loop;
    auto const r = uvco::run(loop, zeroDelayTimer(loop));
    CHECK(r && *r == 1U);
}

TEST_CASE("Timer: tick on a stopped / exhausted timer -> EINVAL") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, notStarted(loop)) == 0);
}

TEST_CASE("Timer: stop requested before tick -> ECANCELED") {
    uvco::Loop loop;
    uvco::Timer timer{loop};
    CHECK(timer.start(1ms, 1ms));
    co2::stop_source source;
    source.request_stop();
    auto const r = uvco::run(loop, tickResult(timer), source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
}

TEST_CASE("Timer: stop during tick (same thread) -> ECANCELED, timer keeps running") {
    uvco::Loop loop;
    co2::stop_source source;
    CHECK(uvco::run(loop, cancelTickKeepsCount(loop, source), source.get_token()) == 0);
}

TEST_CASE("Timer: stop during tick from another thread -> ECANCELED") {
    uvco::Loop loop;
    uvco::Timer timer{loop};
    CHECK(timer.start(10s));
    for (int round = 0; round < 20; ++round) {
        co2::stop_source source;
        std::thread stopper{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{round % 4});
            source.request_stop();
        }};
        auto const r = uvco::run(loop, tickResult(timer), source.get_token());
        stopper.join();
        CHECK(not r && r.error() == Errc::canceled);
    }
}

TEST_CASE("Timer: stop() ends a pending tick with aborted") {
    uvco::Loop loop;
    auto const r = uvco::run(loop, stopWhileTicking(loop));
    CHECK(not r && r.error() == Errc::aborted);
}

TEST_CASE("Timer: close() ends a pending tick with aborted") {
    uvco::Loop loop;
    bool closed = false;
    auto const r = uvco::run(loop, closeWhileTicking(loop, closed));
    CHECK(not r && r.error() == Errc::aborted);
    CHECK(closed);
}

TEST_CASE("Timer: move transfers the handle") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, movedTimer(loop)) == 0);
}

TEST_CASE("Timer: destroying a running timer with no pending op") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, destroyRunningTimer(loop)) == 0);
}

TEST_CASE("Timer: destroyed right before the Loop") {
    uvco::Loop loop;
    {
        uvco::Timer timer{loop};
        CHECK(timer.start(1ms, 1ms));
    } // uv_close 已发起；~Loop 的 UV_RUN_NOWAIT 处理 close 回调
}

TEST_CASE("Timer: unawaited close() still closes") {
    uvco::Loop loop;
    uvco::Timer timer{loop};
    CHECK(timer.start(1ms));
    static_cast<void>(timer.close());
    CHECK(not timer.isOpen());
}

UVCO_TEST_MAIN
