// KeepAlive 释放与 loop 退出的竞争（docs/architecture.md §4.1；借鉴 kotatsu
// tests/async/relay_release）。用 -Wl,--wrap=uv_async_send 把“释放保活”那次唤醒拖慢：
// 若递减计数与唤醒不在同一个锁内，loop 会因另一次唤醒（先前的 post）复查到“计数为零、
// 队列为空”而 unref、退出并销毁 Loop——慢的那次 uv_async_send 随后写入已释放的句柄。
// 正确的实现下，drain() 只能在这次唤醒返回之后才返回。

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include <uv.h>

#include "test.hpp"
#include "uvco/core.hpp"

extern "C" int __real_uv_async_send(uv_async_t* async);

namespace {

std::atomic<std::thread::id> slowThread{};
std::atomic<int> slowSends{0};
std::atomic<bool> slowSendReturned{false};

struct NoopCallback : uvco::detail::LoopCallback {
    NoopCallback() { invoke = [](uvco::detail::LoopCallback*) noexcept {}; }
};

} // namespace

extern "C" int __wrap_uv_async_send(uv_async_t* const async) {
    if (std::this_thread::get_id() != slowThread.load()) return __real_uv_async_send(async);
    // post() 的第一次唤醒立即发出；release 的那次拖慢，给 loop 留出复查的窗口。
    if (slowSends.fetch_add(1) == 0) return __real_uv_async_send(async);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    int const status = __real_uv_async_send(async);
    slowSendReturned = true;
    return status;
}

TEST_CASE("drain() cannot return before the KeepAlive release wakeup was sent") {
    for (int round = 0; round < 5; ++round) {
        slowSends = 0;
        slowSendReturned = false;
        bool returnedBeforeDestroy = false;
        {
            uvco::Loop loop;
            NoopCallback callback;
            uvco::Loop::KeepAlive hold = loop.keepAlive();
            std::thread worker{[&loop, &callback, h = std::move(hold)]() mutable {
                slowThread = std::this_thread::get_id();
                loop.post(&callback); // 立即唤醒 loop
                h.reset();            // 递减 + 被拖慢的唤醒
                slowThread = std::thread::id{};
            }};
            uvco::drain(loop);
            returnedBeforeDestroy = slowSendReturned.load();
            worker.join();
        }
        CHECK(returnedBeforeDestroy);
    }
}

UVCO_TEST_MAIN
