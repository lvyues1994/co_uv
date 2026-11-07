#include <atomic>
#include <thread>
#include <vector>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/core.hpp"

namespace {

auto hop(uvco::Loop& loop, co2::ThreadPool& pool)
    CO2_BEG(co2::Task<bool>, (loop, pool), bool ok{true};) {
    ok = ok && loop.isLoopThread();
    CO2_AWAIT(co2::scheduleOn(pool));
    ok = ok && pool.isWorkerThread() && not loop.isLoopThread();
    CO2_AWAIT(co2::scheduleOn(loop));
    ok = ok && loop.isLoopThread() && not pool.isWorkerThread();
    CO2_RETURN(ok);
}
CO2_END

auto pingPong(uvco::Loop& loop, co2::ThreadPool& pool, int rounds)
    CO2_BEG(co2::Task<int>, (loop, pool, rounds), int i{}; int good{};) {
    for (i = 0; i < rounds; ++i) {
        CO2_AWAIT(co2::scheduleOn(pool));
        if (pool.isWorkerThread()) ++good;
        CO2_AWAIT(co2::scheduleOn(loop));
        if (loop.isLoopThread()) ++good;
    }
    CO2_RETURN(good);
}
CO2_END

// 在线程池上完成：run() 的完成通知必须先回到 loop 线程。
auto finishOnPool(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(7);
}
CO2_END

auto fanOut(uvco::Loop& loop, co2::ThreadPool& pool)
    CO2_BEG(co2::Task<int>, (loop, pool), std::vector<co2::Task<int>> tasks;
            std::vector<int> results; int total{};) {
    for (int k = 0; k < 16; ++k) tasks.push_back(pingPong(loop, pool, 50));
    CO2_AWAIT_SET(results, co2::whenAll(std::move(tasks)));
    for (int r : results) total += r;
    CO2_RETURN(total);
}
CO2_END

} // namespace

TEST_CASE("scheduleOn(loop) from a ThreadPool hops back onto the loop thread") {
    uvco::Loop loop;
    co2::ThreadPool pool{2};
    CHECK(uvco::run(loop, hop(loop, pool)));
}

TEST_CASE("repeated cross-thread ping-pong") {
    uvco::Loop loop;
    co2::ThreadPool pool{4};
    CHECK(uvco::run(loop, pingPong(loop, pool, 2000)) == 4000);
}

TEST_CASE("task completing on another thread finishes run()") {
    uvco::Loop loop;
    co2::ThreadPool pool{2};
    for (int i = 0; i < 200; ++i) CHECK(uvco::run(loop, finishOnPool(pool)) == 7);
}

TEST_CASE("concurrent tasks hopping between pool and loop") {
    uvco::Loop loop;
    co2::ThreadPool pool{4};
    CHECK(uvco::run(loop, fanOut(loop, pool)) == 16 * 50 * 2);
}

UVCO_TEST_MAIN
