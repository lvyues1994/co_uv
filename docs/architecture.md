# uvco 架构设计

uvco 用 [co2](https://github.com/lvyues1994/coro)（C++14 无栈协程库）封装 libuv，
让 libuv 的异步 I/O 能以 `CO2_AWAIT` 的形式书写。

状态：设计草案（v0.1 范围）。

## 1. 设计约束（来自 co2）

- **C++14 + 宏状态机**：跨挂起点的局部变量必须写在 `CO2_BEG` 的帧局部列表中。
- **awaiter 在帧内、地址稳定**：awaiter 内联槽为 8 个指针宽（`CO2_AWAIT_STORAGE_SIZE`），
  超出则退化为一次堆分配。
- **标准销毁契约**：挂起中的协程不能销毁。取消必须是协作式的（`stop_token`），
  被取消的操作要“正常完成”（返回 `UV_ECANCELED`），协程走到终点后再由拥有者销毁。
- **显式调度**：协程在完成它的线程上内联恢复；换线程用 `scheduleOn`。
- co2 v2 暂缺：定时器/超时、`whenAny`、Event/Mutex 等同步原语，由 uvco 自行提供。
  **已定**：这些原语只放在 uvco 内，不回补到 co2。

## 2. 错误模型

- 所有 I/O 操作返回 `uvco::Result<T>`：值或 libuv 错误码（负数 `int`，包装为 `uvco::Errc`）。
  - `[[nodiscard]]`；`operator bool`、`value()`、`error()`、`message()`（`uv_strerror`）。
  - 可转换为 `std::error_code`（自定义 `uv_category()`）。
  - C++14 无 `std::expected`，自行实现轻量版本；`Result<void>` 特化。
- 取消统一表现为 `UV_ECANCELED`。
- 协程体内的 C++ 异常（`bad_alloc`、用户异常）仍走 co2 的异常通道，`Result` 只承载 I/O 错误。

## 3. 线程模型

- 每个线程最多一个 `Loop`；句柄绑定创建它的 loop，只能在该 loop 线程上操作。
- 在错误线程上操作句柄 → co2 契约违规（与 co2 风格一致），而不是静默 UB。
- 跨 loop / 跨线程通过 `CO2_AWAIT(co2::scheduleOn(loop))` 显式跳转。

## 4. 模块分层

```
uvco/
  core/     Result、Errc、Loop（Scheduler）、run()、detail::ReqAwaiter
  timer/    sleep()、Timer
  sync/     Event、Mutex、Channel、whenAny、timeout
  stream/   Stream 基类、Tcp、Pipe、Listener/accept
  udp/      Udp
  fs/       文件操作
```

### 4.1 core

- `Loop`：持有 `uv_loop_t`，实现 `co2::Scheduler`。
  - 绑定构造它的线程（loop 线程），每个线程至多一个 Loop（契约检查）；不可复制、不可移动。
  - `schedule(handle)`：loop 线程内直接入本地队列；其他线程入互斥量保护的远程队列并
    `uv_async_send` 唤醒（在锁内发送，避免唤醒后 Loop 已被销毁）。
  - 本地队列由一个 `uv_idle` 排空，远程队列由 `uv_async` 排空；排空时整批换出再逐个
    resume，新排队的协程留到下一批——不会在 libuv 回调中无限递归，I/O 回调可以穿插。
  - `post(detail::LoopCallback*)`：任意线程投递一个侵入式回调节点（零分配），用于取消（§4.3）。
- `run(loop, task, token = {}) -> T`：loop 版 `syncWait`，启动 Task 并 `uv_run` 直到 Task 完成，
  然后返回值或重抛异常。可选的 `token` 作为整棵任务树的根 stop_token。
  - Task 若在别的线程上完成，完成通知先经 `schedule()` 回到 loop 线程，再停止 `uv_run`。
  - **不**关闭、不排空用户句柄（与 §7 “用户持有 Loop”一致，同一 loop 可多次 `run()`）。
    剩余句柄/请求由用户负责；`~Loop` 时若 `uv_loop_close` 返回 `EBUSY` 即契约违规。
- `detail::ReqAwaiter<Derived, Req, T>`（CRTP）：单次请求 awaiter 模板。
  - `Derived` 提供 `int start(uv_loop_t*)` 与 `Result<T> finish(int status)`；libuv 回调里调用
    `complete(req, status)`。可选 `static constexpr bool cancellable = false` 与自定义 `cancel()`
    （默认 `uv_cancel`）。
  - `uv_xxx_t` 请求体放在 awaiter 内，`req.data` 指向 awaiter；libuv 回调写入结果后直接
    `resume()`。全部在 loop 线程，无需原子量（不复用 `co2::CallbackAwaitable`）。
  - 大请求（`uv_fs_t` 约 440 字节）按 §7 由协程帧直接持有：把 awaiter 声明为帧局部变量后
    `CO2_AWAIT(op)`，其 `operator_co_await() &` 返回一个指针大小的 `AwaiterRef` 放进 co2 的
    内联槽，零堆分配，且 awaiter 可重复等待。直接 await 一个临时 awaiter 也可以，但大请求
    会超出内联槽，产生一次堆分配。

### 4.2 句柄生命周期

`uv_close` 是异步的，而 C++ 析构函数无法等待。方案：

- 句柄对象（`Tcp`、`Timer`…）只持有指向**控制块**的指针；控制块内含 `uv_xxx_t`。
- 析构时：若未关闭则调用 `uv_close`，控制块在 close 回调中释放。
- 提供 `CO2_AWAIT(h.close())` 显式等待关闭完成。
- 句柄只可移动，不可复制。
- 句柄上有挂起中的操作时析构 → 契约违规（与 co2 Task 一致）。

### 4.3 取消

- 每个 awaiter 在 `await_suspend` 中从父 promise 取得 `stop_token` 并注册 `stop_callback`。
- 取消动作：fs → `uv_cancel`；timer → `uv_timer_stop`；stream 读 → `uv_read_stop`；
  accept/recv 等 → 停止监听并以 `UV_ECANCELED` 完成。
- **线程安全**：`stop_callback` 可能在任意线程触发，而 libuv 非线程安全。
  回调中只投递“取消请求”到 loop（经 `uv_async`），真正的取消在 loop 线程执行。
- 已发出、无法取消的请求（例如已在执行的 write）等其自然完成。
- 实现（core）：`await_suspend` 时若已请求停止，直接以 `UV_ECANCELED` 完成、不发起请求；
  否则发起请求后注册 `stop_callback`，回调只 `Loop::post()` 一个侵入式节点（即使已在 loop
  线程也延后执行，避免在 `request_stop()` 调用栈里重入 libuv）；节点在 loop 线程上若请求仍
  在途则调用 `Derived::cancel()`。`await_resume` 先析构 `stop_callback`（会等待其他线程上
  正在执行的回调结束），再撤销尚未执行的节点。

### 4.4 timer

- `sleep(loop, ms) -> Result<void>`：一次性定时器。
- `Timer`：可重复，`CO2_AWAIT(timer.tick())`。
- 是 `timeout` 的基础。

### 4.5 sync（loop 内单线程原语）

- `Event`：set / wait。
- `Mutex`：协程互斥锁，FIFO 唤醒。
- `Channel<T>`：有界/无界，`send` / `recv`，`close()` 后 `recv` 返回 `UV_EOF` 类错误。
- `whenAny(a, b, ...)`：一个分支完成后，用内部 `stop_source` 取消其余分支，
  **等待所有分支以 ECANCELED 结束后**才返回（满足销毁契约）；返回胜出下标与结果。
- `timeout(loop, ms, task)`：基于 `whenAny(task, sleep)`，超时返回 `UV_ETIMEDOUT`。

### 4.6 stream

- `Stream` 基类（`Tcp`、`Pipe`，后续 `Tty`）：
  - `read(buf) -> Result<size_t>`：拉模式。libuv 是推模式（`uv_read_start`），
    仅在有等待者时 start，无等待者时 `uv_read_stop` 形成背压；多余数据进入小缓冲。
    EOF 返回 `UV_EOF`。
  - `write(bufs) -> Result<void>`、`shutdown()`、`close()`。
  - 可选：`chunks() -> co2::AsyncGenerator<Chunk>`。
- `Tcp::connect(addr)`、`Tcp::bind`、`listen(backlog)` + `accept() -> Result<Tcp>`。
- `Pipe::connect(path)`、`Pipe::bind(path)`。
- v0.1 不含 DNS：`connect` 只接受 IP 地址（`sockaddr` / `ip4_addr` / `ip6_addr`）。

### 4.7 udp

- `Udp::bind`、`send(addr, buf) -> Result<void>`、`recv(buf) -> Result<Datagram>`（含来源地址）。
- 读取同 stream：有等待者时 `uv_udp_recv_start`，否则 stop。

### 4.8 fs

- `File::open/read/write/close`、`stat`、`unlink`、`mkdir`、`rename` 等常用操作。
- 基于 `ReqAwaiter<uv_fs_t>`，回调后 `uv_fs_req_cleanup`。
- 取消通过 `uv_cancel`（仅对尚未开始执行的请求有效）。
- 注意：Linux 上 libuv（≥1.45）可能把部分 fs 请求交给 io_uring 执行，此时 `uv_cancel` 返回
  `EBUSY`，请求会自然完成。取消对 fs 只是“尽力而为”；需要确定性测试时设置
  `UV_USE_IO_URING=0`（tests/req_awaiter_tests.cpp 即如此）。

## 5. v0.1 范围（已定）

| 模块 | 包含 |
| --- | --- |
| core | ✅ |
| timer | ✅ |
| sync + whenAny/timeout | ✅ |
| stream（Tcp/Pipe） | ✅ |
| udp | ✅ |
| fs | ✅ |
| dns、work（`uv_queue_work`） | ❌ 后续 |
| process、signal、tty、poll | ❌ 后续 |

## 6. 实现顺序（已定）

1. core：`Result<T>`、`Loop`、`run()`、`ReqAwaiter`
2. timer
3. sync：Event / Mutex / Channel / whenAny / timeout
4. stream：Tcp / Pipe / accept
5. udp
6. fs

## 7. 已定的实现决策

- **大请求存储**：`uv_fs_t` 等大请求放在 uvco 自己的 awaiter 中，由协程帧直接持有，
  不经过 co2 的 8 指针内联槽；零堆分配，不修改 co2 全局配置 `CO2_AWAIT_STORAGE_SIZE`。
- **Loop 所有权**：用户持有 `Loop`；`run(loop, task)` 只负责驱动，同一 loop 可承载多个 Task。
- **依赖引入**：libuv 与 co2 均优先 `find_package`（libuv 还会尝试 pkg-config），找不到时回退到
  FetchContent。co2 的预览版包不接受数值版本号，因此 `find_package(co2 CONFIG)` 不带版本；
  FetchContent 固定到一个提交（`UVCO_CO2_GIT_TAG`）。
- **契约而非异常**：`Result::value()` / `error()` 用错状态、在非 loop 线程调用 libuv 相关接口、
  带着挂起请求析构 awaiter、带着打开的句柄析构 Loop——都是契约违规（走 co2 的契约处理器），
  不抛异常。

## 8. 待定问题

- uvco core 依赖 co2 的若干 `detail` 接口：`TaskAccess`（启动 Task / 取结果）、`stopTokenOf`
  （从 promise 取 stop_token）、`FrameHeader` / `noopDestroy`（run() 的手写完成帧）。
  co2 稳定前需要跟进其变化，或请 co2 提供公开的“启动 Task”扩展点。
- 尚未提供 install / export 规则（目前只支持 `add_subdirectory` / FetchContent 方式使用）。

## 9. 与原设计的偏差（实现 core 时记录）

- §4.1 原写“`run()` 完成后关闭并排空剩余句柄”：与 §7 用户持有 Loop 冲突，改为 `run()` 只驱动；
  句柄清理交给用户，`~Loop` 检查。
- §4.1 本地队列明确使用 `uv_idle`，远程队列使用 `uv_async`；远程队列为互斥量保护的 vector
  而非无锁 MPSC（足够简单，且锁内 `uv_async_send` 解决了 Loop 销毁竞态）。
- §4.1 `ReqAwaiter` 模板参数改为 `<Derived, Req, T>`（CRTP），以便每种请求自定义发起、完成与取消。
- Loop 额外绑定构造线程并限制每线程一个，以便 `isLoopThread()` 检查与 `Loop::current()`。

