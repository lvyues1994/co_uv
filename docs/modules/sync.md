# 模块：sync（loop 内原语）

## 职责

在**单个 loop 线程**内提供协程式同步：手动复位事件、公平互斥锁、FIFO 通道。组合子见 [combinators.md](combinators.md)。

头文件：`<uvco/sync.hpp>`。只能在 loop 线程使用（契约）。跨线程请先 `scheduleOn(loop)`。

## 公开 API

### Event

| 名称 | 说明 |
| --- | --- |
| `Event(loop, initiallySet = false)` | 手动复位。带着等待者销毁是契约违规。 |
| `wait()` | `Result<void>`。已 set 则立即成功；已请求停止即使已 set 也是 `canceled`。 |
| `set()` | 唤醒全部等待者（延后到 loop 恢复）；之后的 `wait()` 立即完成。 |
| `reset()` | 回到未触发。已被 set 唤醒的等待者仍然成功。 |
| `isSet()` / `loop()` | 状态与所属 loop。 |

### Mutex

| 名称 | 说明 |
| --- | --- |
| `Mutex(loop)` | 不可重入：持有者再 `lock()` 会永远等待。 |
| `tryLock()` | 不挂起，成功返回 true。 |
| `lock()` | `Result<void>`，之后手动 `unlock()`。 |
| `scopedLock()` | `Result<Mutex::Guard>`，Guard 只可移动，析构或 `unlock()` 时解锁。 |
| `unlock()` / `isLocked()` | 未持有时 unlock，或持有/仍有等待者时销毁，都是契约违规。 |

公平性：`unlock()` 时若有等待者，所有权**直接移交**给队首（锁保持占用），队首延后恢复；期间新来的 `lock()`/`tryLock()` 不能插队。已获得移交的等待者不会再被取消。`lock()` 在已请求停止时即使锁空闲也返回 `canceled`。

### Channel\<T\>

| 名称 | 说明 |
| --- | --- |
| `Channel(loop)` | 无界。 |
| `Channel(loop, n)` | 有界；`n==0` 为会合（send 等到接收者取走）。 |
| `send(value)` | `Result<void>`。有接收者等待时直接交付，不经缓冲。 |
| `recv()` | `Result<T>`。 |
| `trySend` / `tryRecv` | 不挂起；不能立即完成则为 `EAGAIN`（trySend 失败不移动实参）。 |
| `close()` | 之后 send → `EPIPE`；recv 先取完缓冲再 `EOF`。挂起的接收者以 `EOF` 唤醒，挂起的发送者以 `EPIPE` 唤醒（值丢弃）。重复 close 无效果。 |
| `capacity()` | 容量。 |

发送者、接收者各自严格 FIFO。取消：已请求停止 → `canceled`（即使可以立即完成；值不发送、不接收）。

## 内部要点

等待者队列在 loop 线程上操作。被唤醒者通常延后到下一轮 loop 恢复，与 Timer 的“用户调用栈上不内联恢复等待者”一致，避免在 `set()`/`unlock()`/`send()` 调用栈上重入。

## 限制

- 不是线程安全的，不能当作跨线程队列。跨线程请用 `queueWork` + `scheduleOn`，或在 loop 线程上汇合。
- Mutex 不可重入。
