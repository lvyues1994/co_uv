# 模块：core / Loop

## 职责

提供 Loop 所有权与驱动、错误与结果类型、句柄控制块、单次请求 awaiter、取消投递，以及跨一次完整 libuv 迭代的 `yieldNextTurn`。几乎所有其他模块建立在这一层之上。

头文件：`<uvco/core.hpp>`（含 loop、run/drain、Result/Errc、handle、ReqAwaiter、CancelHook、yield）。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `Loop(scheduleCapacity = 65536)` | 绑定当前线程；每线程至多一个。队列容量构造期预分配，溢出为契约违规。 |
| `Loop::current()` | 当前线程的 Loop，没有则为空。 |
| `raw()` | `uv_loop_t*`。经 raw 自建的句柄由 `~Loop` 兜底关闭。 |
| `isLoopThread()` / `isRunning()` | 线程与 `uv_run` 状态。 |
| `schedule(handle)` | 任意线程把协程排进 loop。 |
| `keepAlive()` | 仅 loop 线程取得；令牌可移到其他线程。跨线程投递回来前必须持有。 |
| `stop()` | 停止当前 `uv_run`（内部给 `run` 用）。 |
| `run(loop, task, token = {})` | 驱动到 Task 完成，返回 `T` 或重抛。不可重入。 |
| `drain(loop)` | 跑到无活动句柄/请求、无排队协程、无 KeepAlive。 |
| `Errc` | libuv 负错误码 + `aborted`。 |
| `strerror` / `errorName` / `make_error_code` | 错误文案与 `std::error_code`（category `"uv"`）。 |
| `Result<T>` / `Result<void>` | 见下表与 [architecture.md §6](../architecture.md)。 |
| `Handle<Block>::isOpen()` / `loop()` / `close()` | 所有句柄共有。`close()` 立即置空所有者。 |
| `yieldNextTurn(loop)` | `Result<void>`。至少跨一个完整 iteration，可取消，Loop 析构时 `aborted`。 |

`Result<T>`：`operator bool` / `hasValue()` 为成功；`value()` 失败、`error()` 成功都是契约违规；`valueOr(fallback)`；`errorCode()`；`message()`。

## 语义

- **完成**：`run` 只保证传入的那一个 Task 结束；其他协程与句柄保持原样。
- **取消**：`run` 的根 token 沿 Task 树借用。`yieldNextTurn` 取消为 `canceled`。
- **关闭**：`~Loop` 顺序见 [architecture.md §5](../architecture.md) 与 [sequences.md](../sequences.md)。带着存活 `KeepAlive` 析构是契约违规。未处理的远程队列亦然。
- **错误**：libuv 失败进 Result；用法错误走契约处理器（`co2::setContractViolationHandler`）。

## 内部要点

- 本地/远程各两条预分配 vector（队列与 batch）。跨线程除句柄外还有侵入式、零分配的 `LoopCallback` 链，供 CancelHook 使用。
- 内部 `uv_async` 默认 unref；`keepAlive_ > 0` 时才 ref。KeepAlive 在锁内递减并在锁内唤醒，避免最后一次唤醒发出前 loop 判定空闲并被销毁。
- `ReqAwaiter`、`WaitSlot`、`HandleBlock`/`HandleRef`、`CancelHook` 的不变量见 [architecture.md §3](../architecture.md)。
- 与 co2 不稳定内部的唯一桥是 `core/co2_compat.hpp`，由 CMake 能力探测把守。

## 限制

- `uv_run` 不可重入，不能在 `run()` 尚未返回时再次 `run` 同一 Loop。
- 调度队列有硬上限，不是无限增长的工作窃取队列。
- 继续依赖 co2 `detail`（TaskAccess、FrameHeader、stopTokenOf 等）。
