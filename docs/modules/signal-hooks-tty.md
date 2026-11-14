# 模块：Signal、相位 hook 与 TTY

## 职责

用统一 watcher 内核提供信号观察、loop 相位回调（Idle/Prepare/Check），以及作为 stream 扩展的终端模式。不把 TTY 做成键盘事件 watcher。

头文件：`<uvco/watcher.hpp>`（Signal、phase），TTY 在 `<uvco/stream.hpp>`。设计背景见 [design-watchers.md](../design-watchers.md)。

## 公开 API

### Signal

| 名称 | 说明 |
| --- | --- |
| `Signal::create(loop)` | 工厂。 |
| `start(signum)` | 成功则开启新纪元（包括重复监视同一 signum）；失败保留旧监视。 |
| `stop()` / `isActive()` / `next()` | `next()` → `Result<SignalEvent>`：`signum`、`count`（饱和）、`saturated`。 |
| `queued()` / `stats()` / `coalescedCount()` | 队列与投递统计。 |

投递策略：饱和计数。计的是 **libuv 回调次数**，不是 POSIX 信号发送次数。每个 watcher、每个 loop 各自计数。

**拒绝 SIGPIPE**：进程级默认已由 Loop 忽略 SIGPIPE（写入已关闭连接应返回 EPIPE）。用 Signal 监视 SIGPIPE 会被拒绝。不要把本模块与外部 `sigaction` 所有者混用；无法保证共存。`SIGCHLD` 仅作通知，子进程回收仍只属于 Process/libuv。

### Idle / Prepare / Check

| 名称 | 说明 |
| --- | --- |
| `HookOptions::{referenced=false}` | hook **默认 unref**，不单独保活 `uv_run`。 |
| `onPrepare(loop, cb, options = {})` | 注册即 start。`cb` 必须 `noexcept`，无 `std::function` 擦除。 |
| `onCheck(loop, cb, options = {})` | 同上。 |
| `onIdle(loop, cb, options = {})` | **创建后是停止的**，需显式 `start()`。活动的 idle 即使 unref 也会让 poll 超时为 0，小心空转吃 CPU。 |
| `PhaseWatcher`（Idle/Prepare/Check） | 高级 awaitable：`start`/`stop`/`next()` → `Result<PhaseEvent>`。策略为 latest-only。无 generator 语法。 |
| `setReferenced(bool)` / `start` / `stop` | 运行中改 ref 不影响“相位 hook 被调用”，只影响是否保活 loop。 |

### TTY

| 名称 | 说明 |
| --- | --- |
| `Tty::open(loop, fd)` 一类工厂 | 通过复制 fd 再交给 libuv（duplicate-only），避免抢走调用者的描述符。 |
| `read` / `write` / `shutdown` / `tryWrite` | 与 stream 相同的拉模式与写取消关流规则。 |
| `setMode(TtyMode::{Normal, Raw, Io})` / `restore()` | 恢复即回到 Normal。 |
| `getSize()` | `Result<TerminalSize>`。 |
| `isReadable` / `isWritable` / `writeQueueSize` | 查询。 |

没有精确的模式快照/租约（ModeGuard）——改模式是进程级/描述符级副作用，需调用者自行恢复。

## 语义

- hook 默认 unref；`referenced` 不等于“会被唤醒”，只影响 loop 存活。
- `next()` 的取消、close、Loop 析构与其他 watcher 相同（`canceled` / `aborted`）。
- 成功 `start` 开启新纪元，旧 `next()` 租约不跟过去。

## 限制

- Signal 的外部 disposition 共存无法保证。
- 无跨平台键盘事件流。TTY ModeGuard 与 Windows/macOS 实机验证仍待完成。
- Idle 可能空转。
