#pragma once

#include <type_traits>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/stop_token.hpp>
#include <co2/task.hpp>

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"

// run(loop, task)：loop 版的 co2::syncWait。
//
//   T value = uvco::run(loop, makeTask());           // 驱动 loop 直到 Task 完成
//   T value = uvco::run(loop, makeTask(), token);    // token 成为 Task 树的根 stop_token
//
// 前置条件：在 loop 线程上调用，且该 loop 没有另一个 run() 正在进行（uv_run 不可重入）。
// Task 在调用线程上内联启动；它可以 scheduleOn 到别的线程再跳回来。若 Task 在别的
// 线程上完成，完成通知会先经 loop.schedule() 转回 loop 线程，再停止 loop。
//
// run() 返回时只保证这个 Task 已经完成；同一 loop 上的其他协程与句柄保持原样
// （Loop 由用户持有，见 docs/architecture.md §7），下一次 run() 会继续驱动它们。
// Task 的异常在这里重抛。

namespace uvco {
namespace detail {

// Task 的续体：一个手写、不占堆的协程帧（与 co2::syncWait 的完成帧同一手法）。
struct RunCompletion {
    co2::detail::FrameHeader header;
    Loop* loop;
    bool finished;

    explicit RunCompletion(Loop& loop_) noexcept
        : header{&co2::detail::noopDestroy, &step, false, false}, loop{&loop_},
          finished{false} {}

    co2::coroutine_handle<> handle() noexcept {
        return co2::coroutine_handle<>::from_address(&header);
    }

    static co2::detail::FrameHeader* step(co2::detail::FrameHeader* const header) {
        static_assert(std::is_standard_layout<RunCompletion>::value,
                      "the completion frame must stay standard-layout");
        auto* const self = reinterpret_cast<RunCompletion*>(header);
        if (not self->loop->isLoopThread()) {
            // 在别的线程完成：把本续体排回 loop 线程再收尾。schedule() 在锁内唤醒
            // loop，之后本线程不再触碰 self 或 loop。
            self->loop->schedule(self->handle());
            return nullptr;
        }
        self->finished = true;
        // 内联完成（run() 还没进入 uv_run）时不留下多余的 uv_stop 标志。
        if (self->loop->isRunning()) self->loop->stop();
        return nullptr;
    }
};

} // namespace detail

template <class T> T run(Loop& loop, co2::Task<T> task, co2::stop_token token = {}) {
    UVCO_CONTRACT_CHECK(static_cast<bool>(task));
    UVCO_CONTRACT_CHECK(loop.isLoopThread());
    UVCO_CONTRACT_CHECK(not loop.isRunning());
    detail::RunCompletion completion{loop};
    co2::detail::TaskAccess::start(task, completion.handle(), std::move(token));
    if (not completion.finished) loop.runUntilStopped(completion.finished);
    return co2::detail::TaskAccess::takeResult(task);
}

} // namespace uvco
