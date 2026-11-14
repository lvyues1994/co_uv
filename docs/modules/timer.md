# 模块：Timer / Sleep

## 职责

提供可重复的定时器句柄，以及一次性的 `sleep`。Timer 使用统一 watcher 内核，投递策略是**累计次数**。

头文件：`<uvco/timer.hpp>`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `Timer(loop)` / `Timer::create(loop)` | 可重复定时器，只可移动。`create` 在 init 失败时返回 Result。 |
| `start(timeout, repeat = 0ms)` | 从调用时刻起 timeout 后首次到期，之后每 repeat 一次（0 表示一次性）。清空累计。 |
| `stop()` | 停止；挂起的 `tick()` 以 `aborted` 结束。清空累计。 |
| `setRepeat` / `repeat()` / `dueIn()` / `isActive()` | 重复间隔、距下次到期（未启动为 0）、是否活动。 |
| `tick()` / `next()` | `Result<uint64_t>`：自上次 tick 以来到期次数（≥1）。awaiter 只含一个指针。 |
| `Sleep(loop, duration)` / `sleep(loop, duration)` | `Result<void>`。计时从**等待时刻**起算（启动前 `uv_update_time`），毫秒粒度，至少等待 duration。 |

## 语义

- **tick()**：已有累计则立即完成并清零；未启动或一次性已取走则立即 `EINVAL`；否则挂起。同一时刻只能有一个 tick（契约）。没人等待时到期会累计，不丢失。
- **取消 tick**：`stop_token` → `canceled`，**计时器继续运行，累计不丢失**。`stop()` / `close()` / `~Loop` → `aborted`。
- **Sleep**：取消为 `canceled`（等待前已请求则不启动计时器）。Loop 析构时以 `aborted` 完成（在途期间登记 LoopResident）。
- **关闭**：普通 Handle 规则。带着挂起的 tick 析构是契约违规。

## 内部要点

- Timer 的 DeliveryPolicy 是累计计数（`LegacyTimerCount`）。`tick()` 的 awaiter 可放入 co2 64B 槽，零分配。
- Sleep 基于 `ReqAwaiter`，每次等待 `uv_timer_init`，到期或取消后先 `uv_close`，close 回调里才恢复，以便销毁 awaiter 内的句柄。代价是每次多一次 close 迭代。右值 Sleep 约 360 字节，会装箱一次；帧局部 Sleep 零装箱。
- 高频周期请用 `Timer`，不要用循环 `sleep`。

## 限制

- 毫秒粒度，不提供硬实时保证。功能测试不把过紧的耗时断言写成契约。
- 回调路径可能内联恢复，`stop()`/`close()` 延后恢复，与 sync 模块的“按唤醒来源区分”策略一致。
