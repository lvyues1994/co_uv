#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
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

// 在另一个线程上延迟请求停止。
struct StopLater {
    StopLater(co2::stop_source& source, std::chrono::milliseconds delay)
        : thread{[&source, delay] {
              std::this_thread::sleep_for(delay);
              source.request_stop();
          }} {}
    ~StopLater() { thread.join(); }
    std::thread thread;
};

auto stopAfter(uvco::Loop& loop, co2::stop_source& source, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<>, (loop, source, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CHECK(r);
    source.request_stop();
}
CO2_END

// ================================================================ Event

auto waitEvent(uvco::Event& event, std::vector<int>& log, int id)
    CO2_BEG(co2::Task<uvco::Result<void>>, (event, log, id), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, event.wait());
    if (r) log.push_back(id);
    CO2_RETURN(r);
}
CO2_END

auto setAfterYield(uvco::Loop& loop, uvco::Event& event, std::vector<int>& log)
    CO2_BEG(co2::Task<>, (loop, event, log)) {
    CO2_AWAIT(co2::scheduleOn(loop));
    CHECK(log.empty());
    event.set();
    CHECK(event.isSet());
    // 延后恢复：set() 返回时等待者还没有运行。
    CHECK(log.empty());
    log.push_back(0);
}
CO2_END

auto eventBroadcast(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::vector<int>>, (loop), uvco::Event event{loop}; std::vector<int> log;
            std::tuple<uvco::Result<void>, uvco::Result<void>, uvco::Result<void>> rs;
            uvco::Result<void> again;) {
    CHECK(not event.isSet());
    CO2_AWAIT_SET(rs, co2::whenAll(waitEvent(event, log, 1), waitEvent(event, log, 2),
                                   waitEvent(event, log, 3), setAfterYield(loop, event, log)));
    CHECK(std::get<0>(rs) && std::get<1>(rs) && std::get<2>(rs));
    // 已置位：立即完成。
    CO2_AWAIT_SET(again, event.wait());
    CHECK(again);
    event.reset();
    CHECK(not event.isSet());
    CO2_RETURN(log);
}
CO2_END

TEST_CASE("Event: set wakes all waiters in FIFO order, deferred") {
    uvco::Loop loop;
    auto const log = uvco::run(loop, eventBroadcast(loop));
    CHECK((log == std::vector<int>{0, 1, 2, 3}));
}

auto waitResult(uvco::Event& event)
    CO2_BEG(co2::Task<uvco::Result<void>>, (event), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, event.wait());
    CO2_RETURN(r);
}
CO2_END

TEST_CASE("Event: initially set completes immediately") {
    uvco::Loop loop;
    uvco::Event event{loop, true};
    CHECK(uvco::run(loop, waitResult(event)));
}

TEST_CASE("Event: stop requested before wait -> ECANCELED even if set") {
    uvco::Loop loop;
    uvco::Event event{loop, true};
    co2::stop_source source;
    source.request_stop();
    auto const r = uvco::run(loop, waitResult(event), source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
}

auto cancelEventWait(uvco::Loop& loop, co2::stop_source& source)
    CO2_BEG(co2::Task<std::vector<int>>, (loop, source), uvco::Event event{loop};
            std::vector<int> log; std::tuple<uvco::Result<void>> rs;) {
    // 等待者继承 run() 的根 token（经 whenAll 转发）；同一 loop 上的另一分支请求停止。
    CO2_AWAIT_SET(rs, co2::whenAll(waitEvent(event, log, 1), stopAfter(loop, source, 3ms)));
    CHECK(not std::get<0>(rs) && std::get<0>(rs).error() == Errc::canceled);
    CHECK(log.empty());
    event.set();
    CO2_RETURN(log);
}
CO2_END

TEST_CASE("Event: cancel during wait (same thread)") {
    uvco::Loop loop;
    co2::stop_source source;
    auto const log = uvco::run(loop, cancelEventWait(loop, source), source.get_token());
    CHECK(log.empty());
}

TEST_CASE("Event: cancel during wait from another thread") {
    for (int i = 0; i < 20; ++i) {
        uvco::Loop loop;
        uvco::Event event{loop};
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i % 3}};
        auto const r = uvco::run(loop, waitResult(event), source.get_token());
        CHECK(not r && r.error() == Errc::canceled);
    }
}

// ================================================================ Mutex

auto lockAndLog(uvco::Loop& loop, uvco::Mutex& mutex, std::vector<int>& log, int id)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, mutex, log, id),
            uvco::Result<uvco::Mutex::Guard> guard;) {
    CO2_AWAIT_SET(guard, mutex.scopedLock());
    if (not guard) CO2_RETURN(uvco::Result<void>{guard.error()});
    CHECK(guard->ownsLock());
    CHECK(mutex.isLocked());
    log.push_back(id);
    // 持锁跨越挂起点：其他人必须等待。
    CO2_AWAIT(co2::scheduleOn(loop));
    CO2_AWAIT(co2::scheduleOn(loop));
    log.push_back(-id);
    guard->unlock();
    CO2_RETURN(uvco::Result<void>{});
}
CO2_END

auto mutexFifo(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::vector<int>>, (loop), uvco::Mutex mutex{loop}; std::vector<int> log;
            std::vector<co2::Task<uvco::Result<void>>> tasks; std::vector<uvco::Result<void>> rs;
            int i{};) {
    for (i = 1; i <= 5; ++i)
        tasks.push_back(lockAndLog(loop, mutex, log, i));
    CO2_AWAIT_SET(rs, co2::whenAll(std::move(tasks)));
    for (auto const& r : rs)
        CHECK(r);
    CHECK(not mutex.isLocked());
    CO2_RETURN(log);
}
CO2_END

TEST_CASE("Mutex: FIFO fairness, mutual exclusion across suspension") {
    uvco::Loop loop;
    auto const log = uvco::run(loop, mutexFifo(loop));
    CHECK((log == std::vector<int>{1, -1, 2, -2, 3, -3, 4, -4, 5, -5}));
}

auto lockThenFlag(uvco::Mutex& mutex, bool& acquired)
    CO2_BEG(co2::Task<>, (mutex, acquired), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, mutex.lock());
    CHECK(r);
    acquired = true;
    mutex.unlock();
}
CO2_END

auto unlockNoBarging(uvco::Loop& loop, uvco::Mutex& mutex, bool& acquired)
    CO2_BEG(co2::Task<>, (loop, mutex, acquired)) {
    CO2_AWAIT(co2::scheduleOn(loop)); // 让等待者先入队
    CHECK(not acquired);
    mutex.unlock(); // 移交给等待者（延后恢复）
    CHECK(mutex.isLocked());
    CHECK(not mutex.tryLock()); // 不能插队
    CHECK(not acquired);
}
CO2_END

auto noBarging(uvco::Loop& loop)
    CO2_BEG(co2::Task<bool>, (loop), uvco::Mutex mutex{loop}; bool acquired{};) {
    CHECK(mutex.tryLock());
    CO2_AWAIT(co2::whenAll(lockThenFlag(mutex, acquired), unlockNoBarging(loop, mutex, acquired)));
    CHECK(not mutex.isLocked());
    CO2_RETURN(acquired);
}
CO2_END

TEST_CASE("Mutex: unlock hands off to the front waiter, no barging") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, noBarging(loop)));
}

auto lockResult(uvco::Mutex& mutex)
    CO2_BEG(co2::Task<uvco::Result<void>>, (mutex), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, mutex.lock());
    CO2_RETURN(r);
}
CO2_END

auto timedLock(uvco::Loop& loop, uvco::Mutex& mutex, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, mutex, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::timeout(loop, d, lockResult(mutex)));
    CO2_RETURN(r);
}
CO2_END

auto unlockAfter(uvco::Loop& loop, uvco::Mutex& mutex, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, mutex, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    mutex.unlock();
    CO2_RETURN(r);
}
CO2_END

auto cancelledWaiterSkipped(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::vector<int>>, (loop), uvco::Mutex mutex{loop}; std::vector<int> log;
            std::tuple<uvco::Result<void>, uvco::Result<void>, uvco::Result<void>> rs;) {
    CHECK(mutex.tryLock());
    CO2_AWAIT_SET(rs, co2::whenAll(timedLock(loop, mutex, 3ms), lockAndLog(loop, mutex, log, 7),
                                   unlockAfter(loop, mutex, 15ms)));
    CHECK(not std::get<0>(rs) && std::get<0>(rs).error() == Errc::timed_out);
    CHECK(std::get<1>(rs));
    CHECK(std::get<2>(rs));
    CHECK(not mutex.isLocked());
    CO2_RETURN(log);
}
CO2_END

TEST_CASE("Mutex: a cancelled waiter leaves the queue; unlock skips it") {
    uvco::Loop loop;
    auto const log = uvco::run(loop, cancelledWaiterSkipped(loop));
    CHECK((log == std::vector<int>{7, -7}));
}

TEST_CASE("Mutex: cancel during lock from another thread; stop-first rule") {
    for (int i = 0; i < 20; ++i) {
        uvco::Loop loop;
        uvco::Mutex mutex{loop};
        CHECK(mutex.tryLock());
        {
            co2::stop_source source;
            StopLater stopper{source, std::chrono::milliseconds{i % 3}};
            auto const r = uvco::run(loop, lockResult(mutex), source.get_token());
            CHECK(not r && r.error() == Errc::canceled);
        }
        mutex.unlock();
        CHECK(not mutex.isLocked());
        co2::stop_source stopped;
        stopped.request_stop();
        auto const r = uvco::run(loop, lockResult(mutex), stopped.get_token());
        CHECK(not r && r.error() == Errc::canceled); // 即使锁空闲
        CHECK(not mutex.isLocked());
    }
}

// ================================================================ Channel

auto produce(uvco::Channel<int>& ch, int from, int to, bool closeAtEnd)
    CO2_BEG(co2::Task<uvco::Result<void>>, (ch, from, to, closeAtEnd), uvco::Result<void> r;
            int i{};) {
    for (i = from; i < to; ++i) {
        CO2_AWAIT_SET(r, ch.send(i));
        if (not r) CO2_RETURN(r);
    }
    if (closeAtEnd) ch.close();
    CO2_RETURN(uvco::Result<void>{});
}
CO2_END

auto consumeAll(uvco::Channel<int>& ch, std::vector<int>& out)
    CO2_BEG(co2::Task<uvco::Result<void>>, (ch, out), uvco::Result<int> v;) {
    for (;;) {
        CO2_AWAIT_SET(v, ch.recv());
        if (not v) break;
        out.push_back(*v);
    }
    CO2_RETURN(uvco::Result<void>{v.error()});
}
CO2_END

auto pipeline(uvco::Loop& loop, std::size_t capacity, int count)
    CO2_BEG(co2::Task<std::vector<int>>, (loop, capacity, count),
            uvco::Channel<int> ch{loop, capacity}; std::vector<int> out;
            std::tuple<uvco::Result<void>, uvco::Result<void>> rs;) {
    CHECK(ch.capacity() == capacity);
    CO2_AWAIT_SET(rs, co2::whenAll(produce(ch, 0, count, true), consumeAll(ch, out)));
    CHECK(std::get<0>(rs));
    CHECK(not std::get<1>(rs) && std::get<1>(rs).error() == Errc::eof);
    CHECK(ch.isClosed());
    CHECK(ch.size() == 0U);
    CO2_RETURN(out);
}
CO2_END

TEST_CASE("Channel: unbounded, bounded and rendezvous preserve order and close drains") {
    std::size_t const capacities[] = {uvco::Channel<int>::unbounded, 1U, 3U, 0U};
    for (auto const capacity : capacities) {
        uvco::Loop loop;
        auto const out = uvco::run(loop, pipeline(loop, capacity, 200));
        CHECK(out.size() == 200U);
        for (std::size_t i = 0; i < out.size(); ++i)
            CHECK(out[i] == static_cast<int>(i));
    }
}

auto recvN(uvco::Loop& loop, uvco::Channel<int>& ch, int n, std::vector<int>& out)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, ch, n, out), uvco::Result<int> v; int i{};) {
    for (i = 0; i < n; ++i) {
        CO2_AWAIT(co2::scheduleOn(loop));
        CHECK(ch.size() <= ch.capacity());
        CO2_AWAIT_SET(v, ch.recv());
        if (not v) CO2_RETURN(uvco::Result<void>{v.error()});
        out.push_back(*v);
    }
    CO2_RETURN(uvco::Result<void>{});
}
CO2_END

auto boundedBlocks(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::vector<int>>, (loop), uvco::Channel<int> ch{loop, 2};
            std::vector<int> out; uvco::Result<int> v;
            std::tuple<uvco::Result<void>, uvco::Result<void>> rs;) {
    CHECK(ch.trySend(1));
    CHECK(ch.trySend(2));
    {
        int three = 3;
        auto const full = ch.trySend(three);
        CHECK(not full && full.error() == Errc::again);
        CHECK(three == 3);
    }
    CHECK(ch.size() == 2U);
    // send(3) 挂起直到 recv 腾出位置；被取走一个后，挂起发送者的值补进缓冲。
    CO2_AWAIT_SET(rs, co2::whenAll(produce(ch, 3, 6, true), recvN(loop, ch, 3, out)));
    CHECK(std::get<0>(rs));
    CHECK(std::get<1>(rs));
    CHECK(ch.isClosed());
    CHECK(ch.size() == 2U);
    {
        auto const closedSend = ch.trySend(9);
        CHECK(not closedSend && closedSend.error() == Errc::broken_pipe);
        auto const first = ch.tryRecv();
        CHECK(first && *first == 4);
    }
    CO2_AWAIT_SET(v, ch.recv());
    CHECK(v && *v == 5);
    CO2_AWAIT_SET(v, ch.recv());
    CHECK(not v && v.error() == Errc::eof);
    {
        auto const eof = ch.tryRecv();
        CHECK(not eof && eof.error() == Errc::eof);
    }
    CO2_RETURN(out);
}
CO2_END

TEST_CASE("Channel: bounded send blocks when full; try* and drain after close") {
    uvco::Loop loop;
    auto const out = uvco::run(loop, boundedBlocks(loop));
    CHECK((out == std::vector<int>{1, 2, 3}));
}

auto closeAfterYield(uvco::Loop& loop, uvco::Channel<int>& ch)
    CO2_BEG(co2::Task<uvco::Result<void>>, (loop, ch)) {
    CO2_AWAIT(co2::scheduleOn(loop));
    ch.close();
    ch.close(); // 幂等
    CO2_RETURN(uvco::Result<void>{});
}
CO2_END

auto closeWakesPending(uvco::Loop& loop)
    CO2_BEG(co2::Task<>, (loop), uvco::Channel<int> rx{loop}; uvco::Channel<int> tx{loop, 0};
            std::vector<int> out;
            std::tuple<uvco::Result<void>, uvco::Result<void>, uvco::Result<void>,
                       uvco::Result<void>, uvco::Result<void>>
                rs;) {
    CO2_AWAIT_SET(rs, co2::whenAll(consumeAll(rx, out), consumeAll(rx, out), closeAfterYield(loop, rx),
                                   produce(tx, 0, 1, false), closeAfterYield(loop, tx)));
    CHECK(not std::get<0>(rs) && std::get<0>(rs).error() == Errc::eof);
    CHECK(not std::get<1>(rs) && std::get<1>(rs).error() == Errc::eof);
    CHECK(not std::get<3>(rs) && std::get<3>(rs).error() == Errc::broken_pipe);
    CHECK(out.empty());
    CHECK(tx.size() == 0U);
}
CO2_END

TEST_CASE("Channel: close wakes pending receivers (EOF) and senders (EPIPE)") {
    uvco::Loop loop;
    uvco::run(loop, closeWakesPending(loop));
}

auto recvResult(uvco::Channel<int>& ch)
    CO2_BEG(co2::Task<uvco::Result<int>>, (ch), uvco::Result<int> v;) {
    CO2_AWAIT_SET(v, ch.recv());
    CO2_RETURN(std::move(v));
}
CO2_END

auto sendResult(uvco::Channel<int>& ch, int value)
    CO2_BEG(co2::Task<uvco::Result<void>>, (ch, value), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, ch.send(value));
    CO2_RETURN(r);
}
CO2_END

TEST_CASE("Channel: cancel pending recv/send from another thread; stop-first rule") {
    for (int i = 0; i < 20; ++i) {
        uvco::Loop loop;
        uvco::Channel<int> ch{loop, 0};
        {
            co2::stop_source source;
            StopLater stopper{source, std::chrono::milliseconds{i % 3}};
            auto const v = uvco::run(loop, recvResult(ch), source.get_token());
            CHECK(not v && v.error() == Errc::canceled);
        }
        {
            co2::stop_source source;
            StopLater stopper{source, std::chrono::milliseconds{i % 3}};
            auto const r = uvco::run(loop, sendResult(ch, 5), source.get_token());
            CHECK(not r && r.error() == Errc::canceled);
        }
        {
            auto const none = ch.tryRecv(); // 被取消的 send 没有送出值
            CHECK(not none && none.error() == Errc::again);
        }
        uvco::Channel<int> buffered{loop, 4};
        CHECK(buffered.trySend(1));
        co2::stop_source stopped;
        stopped.request_stop();
        auto const v = uvco::run(loop, recvResult(buffered), stopped.get_token());
        CHECK(not v && v.error() == Errc::canceled);
        CHECK(buffered.size() == 1U); // 值没有被取走
        auto const r = uvco::run(loop, sendResult(buffered, 2), stopped.get_token());
        CHECK(not r && r.error() == Errc::canceled);
        CHECK(buffered.size() == 1U);
    }
}

using PtrChannel = uvco::Channel<std::unique_ptr<int>>;

auto sendPtr(PtrChannel& ch, int value)
    CO2_BEG(co2::Task<uvco::Result<void>>, (ch, value), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, ch.send(std::unique_ptr<int>{new int{value}}));
    CO2_RETURN(r);
}
CO2_END

auto recvPtr(PtrChannel& ch)
    CO2_BEG(co2::Task<uvco::Result<int>>, (ch), uvco::Result<std::unique_ptr<int>> v;) {
    CO2_AWAIT_SET(v, ch.recv());
    if (not v) CO2_RETURN(uvco::Result<int>{v.error()});
    CO2_RETURN(uvco::Result<int>{**v});
}
CO2_END

auto movePtrs(uvco::Loop& loop)
    CO2_BEG(co2::Task<int>, (loop), PtrChannel ch{loop, 0};
            std::tuple<uvco::Result<void>, uvco::Result<int>> rs;) {
    {
        std::unique_ptr<int> keep{new int{1}};
        auto const r = ch.trySend(std::move(keep));
        CHECK(not r && r.error() == Errc::again);
        CHECK(keep && *keep == 1); // 失败时实参不被移动
    }
    CO2_AWAIT_SET(rs, co2::whenAll(sendPtr(ch, 41), recvPtr(ch)));
    CHECK(std::get<0>(rs));
    CHECK(std::get<1>(rs));
    CO2_RETURN(*std::get<1>(rs) + 1);
}
CO2_END

TEST_CASE("Channel: move-only values through a rendezvous channel") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, movePtrs(loop)) == 42);
}

// ================================================================ whenAny

auto sleepValue(uvco::Loop& loop, std::chrono::milliseconds d, int value,
                std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, d, value, cancelled), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    if (not r) {
        CHECK(r.error() == Errc::canceled);
        cancelled.push_back(value);
        CO2_RETURN(uvco::Result<int>{r.error()});
    }
    CO2_RETURN(uvco::Result<int>{value});
}
CO2_END

auto readyValue(int value) CO2_BEG(co2::Task<uvco::Result<int>>, (value)) {
    CO2_RETURN(uvco::Result<int>{value});
}
CO2_END

// 忽略取消：在 loop 上反复让出，直到 d 过去。
auto stubborn(uvco::Loop& loop, std::chrono::milliseconds d, bool& finished)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, d, finished), Clock::time_point start;) {
    start = Clock::now();
    while (Clock::now() - start < d)
        CO2_AWAIT(co2::scheduleOn(loop));
    finished = true;
    CO2_RETURN(uvco::Result<int>{-1});
}
CO2_END

auto throwAfter(uvco::Loop& loop, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    throw std::runtime_error{"branch failed"};
}
CO2_END

auto failWith(Errc errc) CO2_BEG(co2::Task<uvco::Result<int>>, (errc)) {
    CO2_RETURN(uvco::Result<int>{errc});
}
CO2_END

using Any3 = uvco::Result<uvco::WhenAnyResult<int, int, int>>;

auto firstWins(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<Any3>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled),
                                   sleepValue(loop, 2ms, 2, cancelled),
                                   sleepValue(loop, 10s, 3, cancelled)));
    CO2_RETURN(std::move(r));
}
CO2_END

TEST_CASE("whenAny: first success wins, losers are cancelled and awaited") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, firstWins(loop, cancelled));
    CHECK(msSince(start) < 1000);
    CHECK(r);
    CHECK(r->index() == 1U);
    CHECK(r->get<1>() == 2);
    CHECK(uvco::get<1>(*r) == 2);
    // 两个落败者都已结束（取消回调的先后顺序不作保证）。
    std::sort(cancelled.begin(), cancelled.end());
    CHECK((cancelled == std::vector<int>{1, 3}));
}

auto markStarted(bool& started) CO2_BEG(co2::Task<uvco::Result<int>>, (started)) {
    started = true;
    CO2_RETURN(uvco::Result<int>{-1});
}
CO2_END

auto syncWinner(uvco::Loop& loop, std::vector<int>& cancelled, bool& lateStarted)
    CO2_BEG(co2::Task<std::size_t>, (loop, cancelled, lateStarted),
            uvco::Result<uvco::WhenAnyResult<int, int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled), readyValue(5),
                                   markStarted(lateStarted)));
    CHECK(r);
    CHECK(r->get<1>() == 5);
    CO2_RETURN(r->index());
}
CO2_END

TEST_CASE("whenAny: synchronous winner; branches after it are never started") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    bool lateStarted = false;
    auto const start = Clock::now();
    CHECK(uvco::run(loop, syncWinner(loop, cancelled, lateStarted)) == 1U);
    CHECK(msSince(start) < 1000);
    CHECK((cancelled == std::vector<int>{1})); // 只有胜者之前启动的分支被取消
    CHECK(not lateStarted);
}

auto slowLoser(uvco::Loop& loop, bool& finished, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<std::size_t>, (loop, finished, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(stubborn(loop, 40ms, finished),
                                   sleepValue(loop, 2ms, 2, cancelled)));
    CHECK(finished); // 等到忽略取消的分支自己结束
    CHECK(r);
    CHECK(r->get<1>() == 2);
    CO2_RETURN(r->index());
}
CO2_END

TEST_CASE("whenAny: waits for losers that ignore cancellation") {
    uvco::Loop loop;
    bool finished = false;
    std::vector<int> cancelled;
    auto const start = Clock::now();
    CHECK(uvco::run(loop, slowLoser(loop, finished, cancelled)) == 1U);
    CHECK(msSince(start) >= 40);
    CHECK(finished);
}

auto allSleep(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled),
                                   sleepValue(loop, 10s, 2, cancelled)));
    if (not r) CO2_RETURN(uvco::Result<int>{r.error()});
    CO2_RETURN(uvco::Result<int>{r->index() == 0U ? r->get<0>() : r->get<1>()});
}
CO2_END

TEST_CASE("whenAny: parent cancellation (other thread) propagates to all branches") {
    for (int i = 0; i < 10; ++i) {
        uvco::Loop loop;
        std::vector<int> cancelled;
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i % 4}};
        auto const start = Clock::now();
        auto const r = uvco::run(loop, allSleep(loop, cancelled), source.get_token());
        CHECK(msSince(start) < 1000);
        CHECK(not r && r.error() == Errc::canceled);
        // 停止早于 whenAny 挂起：一个分支都不启动；停止落在启动途中：先启动的分支以
        // ECANCELED 结束即决出比赛，之后的分支不再启动。启动了的分支都被取消。
        CHECK(cancelled.size() <= 2U);
    }
}

auto alreadyStopped(bool& started) CO2_BEG(co2::Task<uvco::Result<int>>, (started),
                                           uvco::Result<uvco::WhenAnyResult<int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(markStarted(started)));
    CHECK(not r);
    CO2_RETURN(uvco::Result<int>{r.error()});
}
CO2_END

TEST_CASE("whenAny: stop already requested -> ECANCELED, no branch starts") {
    uvco::Loop loop;
    co2::stop_source source;
    source.request_stop();
    bool started = false;
    auto const r = uvco::run(loop, alreadyStopped(started), source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
    CHECK(not started);
}

auto winnerThrows(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<std::size_t>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled), throwAfter(loop, 1ms)));
    CO2_RETURN(r->index());
}
CO2_END

TEST_CASE("whenAny: an exception ends the race and is rethrown after all branches finish") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, winnerThrows(loop, cancelled)));
    } catch (std::runtime_error const& error) {
        caught = std::string{error.what()} == "branch failed";
    }
    CHECK(caught);
    CHECK((cancelled == std::vector<int>{1}));
}

auto loserThrows(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<int>, (loop, cancelled), uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    // throwAfter 在 sleep 被取消后照样抛出：异常优先级最高，即使来自落败者。
    CO2_AWAIT_SET(r, uvco::whenAny(throwAfter(loop, 10s), sleepValue(loop, 1ms, 7, cancelled)));
    CO2_RETURN(r->get<1>());
}
CO2_END

TEST_CASE("whenAny: a loser's exception beats the winner's value") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, loserThrows(loop, cancelled)));
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

// d 后以 errc 失败；被取消时返回 onCancel。
auto failAfter(uvco::Loop& loop, std::chrono::milliseconds d, Errc errc, Errc onCancel)
    CO2_BEG(co2::Task<uvco::Result<std::string>>, (loop, d, errc, onCancel),
            uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CO2_RETURN(uvco::Result<std::string>{r ? errc : onCancel});
}
CO2_END

auto errorEnds(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, std::string, std::string>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled),
                                   failAfter(loop, 1ms, Errc::invalid_argument, Errc::canceled),
                                   failAfter(loop, 10s, Errc::busy, Errc::canceled)));
    CO2_RETURN(r ? uvco::Result<int>{0} : uvco::Result<int>{r.error()});
}
CO2_END

TEST_CASE("whenAny: a non-cancel error ends the race and propagates") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, errorEnds(loop, cancelled));
    CHECK(msSince(start) < 1000);
    CHECK(not r && r.error() == Errc::invalid_argument);
    CHECK((cancelled == std::vector<int>{1}));
}

auto loserFails(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<std::string, int>> r;) {
    // 胜者先成功；落败者在被取消途中以非取消错误结束：错误照样传出（同 kotatsu）。
    CO2_AWAIT_SET(r, uvco::whenAny(failAfter(loop, 10s, Errc::busy, Errc::broken_pipe),
                                   sleepValue(loop, 1ms, 7, cancelled)));
    CO2_RETURN(r ? uvco::Result<int>{r->get<1>()} : uvco::Result<int>{r.error()});
}
CO2_END

TEST_CASE("whenAny: an error from a loser during cancellation beats the winner") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const r = uvco::run(loop, loserFails(loop, cancelled));
    CHECK(not r && r.error() == Errc::broken_pipe);
}

auto errorThenThrow(uvco::Loop& loop)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop),
            uvco::Result<uvco::WhenAnyResult<std::string, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(failAfter(loop, 1ms, Errc::busy, Errc::canceled),
                                   throwAfter(loop, 10s)));
    CO2_RETURN(uvco::Result<int>{r ? 0 : 1});
}
CO2_END

TEST_CASE("whenAny: an exception beats an earlier error") {
    uvco::Loop loop;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, errorThenThrow(loop)));
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

auto branchCancels(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, cancelled),
            uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    // 分支自己以 ECANCELED 结束（不是错误）：比赛以取消结束。
    CO2_AWAIT_SET(r, uvco::whenAny(sleepValue(loop, 10s, 1, cancelled), failWith(Errc::canceled)));
    CO2_RETURN(r ? uvco::Result<int>{0} : uvco::Result<int>{r.error()});
}
CO2_END

TEST_CASE("whenAny: a branch ending with ECANCELED decides the race as cancelled") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const r = uvco::run(loop, branchCancels(loop, cancelled));
    CHECK(not r && r.error() == Errc::canceled);
    CHECK((cancelled == std::vector<int>{1}));
}

// ---------------------------------------------------------------- 异构与 awaitable

auto plainInt(uvco::Loop& loop, std::chrono::milliseconds d, int value)
    CO2_BEG(co2::Task<int>, (loop, d, value), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    CO2_RETURN(value);
}
CO2_END

auto plainVoid(uvco::Loop& loop, std::chrono::milliseconds d, bool& done)
    CO2_BEG(co2::Task<>, (loop, d, done), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    if (r) done = true;
}
CO2_END

using Mixed = uvco::WhenAnyResult<int, void, int, void, std::string>;

auto mixed(uvco::Loop& loop, uvco::Channel<std::string>& ch, std::vector<int>& cancelled,
           int winner, bool& voidDone)
    CO2_BEG(co2::Task<std::size_t>, (loop, ch, cancelled, winner, voidDone),
            uvco::Result<Mixed> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(plainInt(loop, winner == 0 ? 1ms : 10s, 10),
                                   plainVoid(loop, winner == 1 ? 1ms : 10s, voidDone),
                                   sleepValue(loop, winner == 2 ? 1ms : 10s, 30, cancelled),
                                   uvco::sleep(loop, winner == 3 ? 1ms : 10s), ch.recv()));
    CHECK(r);
    switch (r->index()) {
    case 0: CHECK(r->get<0>() == 10); break;
    case 1: r->get<1>(); CHECK(voidDone); break;
    case 2: CHECK(r->get<2>() == 30); break;
    case 3: break;
    default: CHECK(r->get<4>() == "hello"); break;
    }
    CO2_RETURN(r->index());
}
CO2_END

auto sendLater(uvco::Loop& loop, uvco::Channel<std::string>& ch)
    CO2_BEG(co2::Task<>, (loop, ch), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 1ms));
    CO2_AWAIT_SET(r, ch.send(std::string{"hello"}));
}
CO2_END

auto mixedWithSender(uvco::Loop& loop, uvco::Channel<std::string>& ch,
                     std::vector<int>& cancelled, bool& voidDone)
    CO2_BEG(co2::Task<std::size_t>, (loop, ch, cancelled, voidDone),
            std::tuple<std::size_t> rs;) {
    CO2_AWAIT_SET(rs, co2::whenAll(mixed(loop, ch, cancelled, 4, voidDone), sendLater(loop, ch)));
    CO2_RETURN(std::get<0>(rs));
}
CO2_END

TEST_CASE("whenAny: heterogeneous branches (Task<int>, Task<void>, Result, sleep, recv)") {
    for (int winner = 0; winner < 4; ++winner) {
        uvco::Loop loop;
        uvco::Channel<std::string> ch{loop};
        std::vector<int> cancelled;
        bool voidDone = false;
        auto const start = Clock::now();
        CHECK(uvco::run(loop, mixed(loop, ch, cancelled, winner, voidDone)) ==
              static_cast<std::size_t>(winner));
        CHECK(msSince(start) < 1000);
        CHECK(cancelled.size() == (winner == 2 ? 0U : 1U));
    }
    uvco::Loop loop;
    uvco::Channel<std::string> ch{loop};
    std::vector<int> cancelled;
    bool voidDone = false;
    CHECK(uvco::run(loop, mixedWithSender(loop, ch, cancelled, voidDone)) == 4U);
    CHECK((cancelled == std::vector<int>{30}));
    CHECK(not voidDone);
}

auto recvOrSleep(uvco::Loop& loop, uvco::Channel<int>& ch)
    CO2_BEG(co2::Task<int>, (loop, ch), uvco::Result<uvco::WhenAnyResult<int, void>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(ch.recv(), uvco::sleep(loop, 10s)));
    CHECK(r);
    CO2_RETURN(r->index() == 0U ? r->get<0>() : -1);
}
CO2_END

auto recvOrSleepResult(uvco::Loop& loop, uvco::Channel<int>& ch)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, ch),
            uvco::Result<uvco::WhenAnyResult<int, void>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(ch.recv(), uvco::sleep(loop, 10s)));
    CO2_RETURN(r ? uvco::Result<int>{r->index() == 0U ? r->get<0>() : -1}
                 : uvco::Result<int>{r.error()});
}
CO2_END

TEST_CASE("whenAny: awaitables directly; a ready recv wins and the timer never starts") {
    uvco::Loop loop;
    uvco::Channel<int> ch{loop, 1};
    CHECK(ch.trySend(42));
    auto const start = Clock::now();
    CHECK(uvco::run(loop, recvOrSleep(loop, ch)) == 42);
    CHECK(msSince(start) < 1000);
    ch.close(); // 关闭：recv 返回 EOF 错误 → 比赛以 EOF 结束
    auto const r = uvco::run(loop, recvOrSleepResult(loop, ch));
    CHECK(not r && r.error() == Errc::eof);
}

auto rangeAny(uvco::Loop& loop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<std::size_t>, (loop, cancelled),
            std::vector<co2::Task<uvco::Result<int>>> tasks;
            uvco::Result<uvco::WhenAnyRangeResult<int>> r{Errc::canceled}; int i{};) {
    for (i = 0; i < 16; ++i)
        tasks.push_back(sleepValue(loop, i == 9 ? 1ms : 10s, i, cancelled));
    CO2_AWAIT_SET(r, uvco::whenAny(std::move(tasks)));
    CHECK(r);
    CHECK(r->value == 9);
    CO2_RETURN(r->index);
}
CO2_END

TEST_CASE("whenAny: vector overload") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    CHECK(uvco::run(loop, rangeAny(loop, cancelled)) == 9U);
    CHECK(cancelled.size() == 15U);
}

auto rangeSleeps(uvco::Loop& loop)
    CO2_BEG(co2::Task<std::size_t>, (loop), std::vector<uvco::Sleep> sleeps;
            uvco::Result<uvco::WhenAnyRangeResult<void>> r{Errc::canceled}; int i{};) {
    for (i = 0; i < 4; ++i)
        sleeps.push_back(uvco::sleep(loop, i == 2 ? 1ms : 10s));
    CO2_AWAIT_SET(r, uvco::whenAny(std::move(sleeps)));
    CHECK(r);
    CO2_RETURN(r->index);
}
CO2_END

TEST_CASE("whenAny: vector of awaitables with a void success") {
    uvco::Loop loop;
    auto const start = Clock::now();
    CHECK(uvco::run(loop, rangeSleeps(loop)) == 2U);
    CHECK(msSince(start) < 1000);
}

auto onPool(co2::ThreadPool& pool, std::chrono::milliseconds d, int value)
    CO2_BEG(co2::Task<uvco::Result<int>>, (pool, d, value)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    std::this_thread::sleep_for(d);
    CO2_RETURN(uvco::Result<int>{value});
}
CO2_END

auto crossThread(uvco::Loop& loop, co2::ThreadPool& pool, std::vector<int>& cancelled,
                 bool poolWins)
    CO2_BEG(co2::Task<std::size_t>, (loop, pool, cancelled, poolWins),
            uvco::Result<uvco::WhenAnyResult<int, int>> r;) {
    CO2_AWAIT_SET(r, uvco::whenAny(onPool(pool, poolWins ? 0ms : 5ms, 1),
                                   sleepValue(loop, poolWins ? 10s : 0ms, 2, cancelled)));
    CO2_AWAIT(co2::scheduleOn(loop)); // 可能在线程池上恢复
    CHECK(loop.isLoopThread());
    CHECK(r);
    CO2_RETURN(r->index());
}
CO2_END

TEST_CASE("whenAny: branches on another thread; cancellation crosses threads") {
    co2::ThreadPool pool{2};
    for (int i = 0; i < 20; ++i) {
        uvco::Loop loop;
        std::vector<int> cancelled;
        CHECK(uvco::run(loop, crossThread(loop, pool, cancelled, true)) == 0U);
        // 线程池分支可能在第二个分支启动前就已胜出：此时第二个分支不再启动。
        CHECK((cancelled == std::vector<int>{2} || cancelled.empty()));
        cancelled.clear();
        CHECK(uvco::run(loop, crossThread(loop, pool, cancelled, false)) == 1U);
    }
}

// ================================================================ timeout

auto timed(uvco::Loop& loop, std::chrono::milliseconds limit, co2::Task<uvco::Result<int>> task)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, limit, task), uvco::Result<int> r{Errc::canceled};) {
    CO2_AWAIT_SET(r, uvco::timeout(loop, limit, std::move(task)));
    CO2_RETURN(std::move(r));
}
CO2_END

TEST_CASE("timeout: hit returns ETIMEDOUT and cancels the task") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, timed(loop, 10ms, sleepValue(loop, 10s, 1, cancelled)));
    auto const elapsed = msSince(start);
    CHECK(not r && r.error() == Errc::timed_out);
    CHECK(elapsed >= 9 && elapsed < 1000);
    CHECK((cancelled == std::vector<int>{1}));
}

TEST_CASE("timeout: miss returns the task's value without waiting for the timer") {
    uvco::Loop loop;
    std::vector<int> cancelled;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, timed(loop, 10s, sleepValue(loop, 1ms, 7, cancelled)));
    CHECK(msSince(start) < 1000);
    CHECK(r && *r == 7);
    auto const e = uvco::run(loop, timed(loop, 10s, failWith(Errc::invalid_argument)));
    CHECK(not e && e.error() == Errc::invalid_argument);
}

TEST_CASE("timeout: task ignoring cancellation delays the ETIMEDOUT result") {
    uvco::Loop loop;
    bool finished = false;
    auto const start = Clock::now();
    auto const r = uvco::run(loop, timed(loop, 5ms, stubborn(loop, 40ms, finished)));
    CHECK(not r && r.error() == Errc::timed_out);
    CHECK(finished);
    CHECK(msSince(start) >= 40);
}

TEST_CASE("timeout: parent cancellation -> ECANCELED, not ETIMEDOUT") {
    for (int i = 0; i < 10; ++i) {
        uvco::Loop loop;
        std::vector<int> cancelled;
        co2::stop_source source;
        StopLater stopper{source, std::chrono::milliseconds{i % 3}};
        auto const r = uvco::run(loop, timed(loop, 10s, sleepValue(loop, 10s, 1, cancelled)),
                                 source.get_token());
        CHECK(not r && r.error() == Errc::canceled);
    }
}

TEST_CASE("timeout: exception from the task propagates") {
    uvco::Loop loop;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, timed(loop, 10s, throwAfter(loop, 1ms))));
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

auto timedRecv(uvco::Loop& loop, uvco::Channel<int>& ch, std::chrono::milliseconds limit)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, ch, limit), uvco::Result<int> r{Errc::canceled};) {
    CO2_AWAIT_SET(r, uvco::timeout(loop, limit, ch.recv()));
    CO2_RETURN(std::move(r));
}
CO2_END

auto timedPlain(uvco::Loop& loop) CO2_BEG(co2::Task<uvco::Result<int>>, (loop),
                                          uvco::Result<int> r{Errc::canceled};
                                          uvco::Result<void> s;) {
    CO2_AWAIT_SET(r, uvco::timeout(loop, 10s, plainInt(loop, 1ms, 5))); // Task<int> → Result<int>
    CHECK(r && *r == 5);
    CO2_AWAIT_SET(s, uvco::timeout(loop, 10s, uvco::sleep(loop, 1ms))); // Result<void>
    CHECK(s);
    CO2_AWAIT_SET(s, uvco::timeout(loop, 1ms, uvco::sleep(loop, 10s)));
    CHECK(not s && s.error() == Errc::timed_out);
    CO2_RETURN(std::move(r));
}
CO2_END

TEST_CASE("timeout: any awaitable (Channel::recv, sleep, plain Task<int>)") {
    uvco::Loop loop;
    uvco::Channel<int> ch{loop, 1};
    auto const start = Clock::now();
    auto const miss = uvco::run(loop, timedRecv(loop, ch, 5ms));
    CHECK(not miss && miss.error() == Errc::timed_out);
    CHECK(ch.trySend(9));
    auto const hit = uvco::run(loop, timedRecv(loop, ch, 10s));
    CHECK(hit && *hit == 9);
    CHECK(uvco::run(loop, timedPlain(loop)));
    CHECK(msSince(start) < 1000);
}

auto untilEvent(uvco::Loop& loop, uvco::Event& stop, std::vector<int>& cancelled,
                std::chrono::milliseconds work)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, stop, cancelled, work),
            uvco::Result<int> r{Errc::canceled};) {
    CO2_AWAIT_SET(r, uvco::runUntil(sleepValue(loop, work, 1, cancelled), stop.wait()));
    CO2_RETURN(std::move(r));
}
CO2_END

auto setLater(uvco::Loop& loop, uvco::Event& event, std::chrono::milliseconds d)
    CO2_BEG(co2::Task<>, (loop, event, d), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, d));
    event.set();
}
CO2_END

auto untilEventSetLater(uvco::Loop& loop, uvco::Event& stop, std::vector<int>& cancelled)
    CO2_BEG(co2::Task<uvco::Result<int>>, (loop, stop, cancelled),
            std::tuple<uvco::Result<int>> rs{uvco::Result<int>{Errc::canceled}};) {
    CO2_AWAIT_SET(rs, co2::whenAll(untilEvent(loop, stop, cancelled, 10s), setLater(loop, stop, 2ms)));
    CO2_RETURN(std::move(std::get<0>(rs)));
}
CO2_END

TEST_CASE("runUntil: a stop cancels the operation -> ECANCELED; otherwise the value") {
    {
        uvco::Loop loop;
        uvco::Event stop{loop};
        std::vector<int> cancelled;
        auto const start = Clock::now();
        auto const r = uvco::run(loop, untilEventSetLater(loop, stop, cancelled));
        CHECK(not r && r.error() == Errc::canceled);
        CHECK((cancelled == std::vector<int>{1}));
        CHECK(msSince(start) < 1000);
    }
    {
        uvco::Loop loop;
        uvco::Event stop{loop};
        std::vector<int> cancelled;
        auto const r = uvco::run(loop, untilEvent(loop, stop, cancelled, 1ms));
        CHECK(r && *r == 1);
    }
    {
        // stop 已满足：先启动的 stop 同步结束，主操作根本不启动。
        uvco::Loop loop;
        uvco::Event stop{loop, true};
        std::vector<int> cancelled;
        auto const r = uvco::run(loop, untilEvent(loop, stop, cancelled, 10s));
        CHECK(not r && r.error() == Errc::canceled);
        CHECK(cancelled.empty());
    }
}


} // namespace

UVCO_TEST_MAIN
