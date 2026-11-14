# 模块：queueWork

## 职责

把闭包投递到 libuv 全局线程池（与 fs、getaddrinfo 共享），完成后回到 loop 线程交付 `Result<R>`。

头文件：`<uvco/work.hpp>`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `queueWork(loop, fn)` | 返回可等待的 Work awaiter，结果 `Result<R>`。`fn` 可以是 `R()` 或 `R(co2::stop_token)`。 |

## 语义

- **排队中取消**：先 `uv_cancel`；成功则 fn 不运行，返回 `canceled`。
- **运行中**：不强制中断。token 仅供协作轮询。fn 若自己注册 `stop_callback`，必须 `noexcept`。
- **晚到取消**：callable **一旦算完就返回实际结果**（包括 `void`），不因为之后才看到的 stop 而丢弃结果。取消不回滚已经发生的副作用。
- **异常**：worker 体内或结果构造的异常保存为 `exception_ptr`，在 after-work 排空后于 loop 线程重抛，优先于取消。
- 已请求停止且尚未 start 时，遵循 ReqAwaiter 的先停止规则（不入队）。

## 内部要点

取消状态懒创建：构造时使用 `nostopstate`；只有签名接受 token 的 callable 在真正 `start` 时才构造 `stop_source`。不带 token 的任务全程不为取消状态分配。

Work 是 `ReqAwaiter`，右值可能因体积装箱；需要零装箱时把 awaiter 放进协程帧局部。

## 限制

- 不能强制打断已经在跑的 fn。忽略协作取消且永不返回的任务会挡住 `drain` / 关闭。
- 全局线程池大小由环境变量 `UV_THREADPOOL_SIZE` 等决定，与 fs/DNS 争用。
- 返回 `canceled` 并不表示“什么都没发生”。
