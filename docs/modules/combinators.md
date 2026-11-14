# 模块：组合子

## 职责

在一组 awaitable / `Task` 上做结构化并发：任一完成（`whenAny`）、全部成功（`whenAllResults`）、限时（`timeout`）以及“等到停止条件”（`runUntil`）。它们返回 awaitable（不一定是新的 Task 帧），结果映射不额外占一个用户可见的组合帧以外的成本（内部为每分支准备续体）。

头文件：`<uvco/sync.hpp>`（`when_any.hpp`、`when_all_results.hpp`、`timeout.hpp`）。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `whenAny(a, b, …)` | 异构，任意 awaitable（Task 须移入）。成功类型是带索引的变体（`WhenAnyResult`），`Result<U>` 解包为 `U`，void 无载荷。 |
| `whenAny(vector<A>)` | 同类型非空范围，成功值为 `{index, value}`（void 范围只有 index）。 |
| `whenAllResults(a, b, …)` | 全部成功后得到 `Result<tuple<成功值…>>`，void 元素省略。 |
| `whenAllResults(vector<A>)` | `Result<vector<T>>`；void 范围得到 `Result<void>`；空范围成功。 |
| `timeout(loop, duration, op)` | `whenAny(op, sleep(loop, duration))` 再映射：op 先成功则取其值；睡眠先到则 `timed_out`。 |
| `runUntil(op, stops…)` | `whenAny(stops…, op)`：任一 stop **成功**则取消 op，结果为 `canceled`。stop 的成功值丢弃；stop 的非取消错误照常传出。 |

`co2::whenAll` 完全不变，仍把 `Result<T>` 当作普通值，不会把业务错误当成失败。

## 语义

- **启动顺序**：`whenAny` / `whenAllResults` 从左到右启动。某个分支失败后，尚未启动的分支直接销毁。`timeout` 先启动 op，op 同步完成则计时器根本不启动。`runUntil` 先启动 stops，已满足的 stop 让 op 不启动。
- **完成**：必须等**所有已启动**分支结束才把结果交给等待者（结构化并发）。忽略取消且永不结束的分支会挡住整个组合子。
- **优先级**：异常 > 首个业务错误 > 取消。分支结果在取出/移动/构造时抛异常，续体仍是 noexcept：异常记入该分支，取消其余分支，等全部已启动分支结束后在 `await_resume` 重抛；落败者的异常也不丢。
- **取消**：父 token 已请求则尽早 `canceled`，并停止已启动兄弟。`timeout` 的父取消是 `canceled`，不是 `timed_out`。
- **timeout 与写**：若 `op` 是挂起的 stream `write`，超时会取消该 write，从而**关闭整个流**（见 [stream.md](stream.md)）。

## 内部要点

复用 `RaceState`：记录第一个异常、第一个错误、决定者，`request_stop()` 在递减剩余计数之前发出。每分支一个固定布局的续体帧与 `ResultStorage` / `exception_ptr`。

## 限制

- “只提取胜者、丢弃其余成功值”的专用 API 尚未批准；需要其余值时请自己保存。
- 范围版 `whenAny` 要求非空。
- 不提供跨线程汇合以外的额外取消传播原语。
