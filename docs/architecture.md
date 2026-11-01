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
- 实现（`core/handle.hpp`，Timer / Stream / Udp 共用）：
  - `detail::HandleBlock`：堆上控制块（虚析构），`uv_handle_t::data` 一律存 `HandleBlock*`。
    派生类提供 `uvHandle()` 与 `abortPending()`；挂起操作用 `opBegin()/opEnd()` 计数。
    控制块在“close 回调已执行 **且** 没有在途操作”时才释放，因此被中止的操作恢复时控制块仍有效。
  - `detail::Handle<Block>`：只可移动的所有者基类。析构 = 若有挂起操作则契约违规，否则 `uv_close`。
  - `close()` 返回 `CloseAwaiter`（`[[nodiscard]]`），并**立即置空**所有者（之后 `isOpen()==false`，
    析构不再触碰控制块）。等待它直到 close 回调执行；未被等待的 `CloseAwaiter` 在析构时仍会发起关闭。
    对空句柄 `close()` 为空操作。
  - 扩展：`close()` 时若有挂起操作，以 `UV_ECANCELED` 中止它们（经 `loop.schedule` 延后恢复），
    而不是契约违规——显式 close 是合法的“取消一切”手段；只有直接析构才算违规。
  - 句柄可以在 Loop 析构之前一刻销毁：`~Loop` 会跑一次 `uv_run(NOWAIT)` 处理待关闭句柄。

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
- 该机制抽成 `detail::CancelHook`（`core/cancel.hpp`）：`arm(loop, token, fn, ctx)` /
  `disarm()`，`ReqAwaiter` 与句柄控制块（如 `TimerBlock`）共用。句柄操作把 CancelHook 放在控制块里，
  使 awaiter 只有一个指针大小、无分配。

### 4.4 timer

- `sleep(loop, ms) -> Result<void>`：一次性定时器。
- `Timer`：可重复，`CO2_AWAIT(timer.tick())`。
- 是 `timeout` 的基础。
- 实现（`include/uvco/timer/`）：
  - 时长统一用 `std::chrono::milliseconds`（负值契约违规）。启动前调用 `uv_update_time`，
    计时从调用时刻起算，而非从本次迭代开始时缓存的 `uv_now` 起算（否则首个 sleep 可能提前结束）。
  - `sleep()` 返回 `Sleep`，基于 `ReqAwaiter`，内嵌 `uv_timer_t`；到期或取消后先 `uv_close`，
    在 close 回调里恢复（因此每次 sleep 多一次迭代）。`CO2_AWAIT(sleep(loop, d))`（右值）有一次堆分配；
    帧局部 `Sleep s{loop, d}; CO2_AWAIT(s);` 零分配且可复用（`setDuration`）。
  - `Timer t{loop}; t.start(timeout, repeat)`；`stop()`、`setRepeat()/repeat()`、`dueIn()`、`isActive()`。
  - `tick() -> Result<std::uint64_t>`：返回自上次 tick 以来的到期次数（无人等待时累计，类似 timerfd）；
    已有累计则立即返回。未启动 / 一次性已耗尽 → `UV_EINVAL`。同一时刻只允许一个等待者（契约）。
  - stop_token 取消 tick → `UV_ECANCELED`，**定时器继续运行**；`stop()` / `close()` 使挂起的 tick
    以 `UV_ECANCELED` 结束。

### 4.5 sync（loop 内单线程原语）

- `Event`：set / wait。
- `Mutex`：协程互斥锁，FIFO 唤醒。
- `Channel<T>`：有界/无界，`send` / `recv`，`close()` 后 `recv` 返回 `UV_EOF` 类错误。
- `whenAny(a, b, ...)`：一个分支完成后，用内部 `stop_source` 取消其余分支，
  **等待所有分支以 ECANCELED 结束后**才返回（满足销毁契约）；返回胜出下标与结果。
- `timeout(loop, ms, task)`：基于 `whenAny(task, sleep)`，超时返回 `UV_ETIMEDOUT`。

实现（`include/uvco/sync/`，聚合头 `uvco/sync.hpp`）：

- 共同规则
  - Event / Mutex / Channel 只能在所属 loop 线程上使用（契约），不加锁。
  - 等待者按到达顺序排在 `detail::WaitQueue` 中（侵入式双链表）；节点内嵌在 awaiter 里（awaiter 在
    co2 的 awaiter 槽中、地址稳定），不挂起的快速路径与不可停止的等待零分配；只有以可停止的
    token 挂起时才分配一个 `CancelHook`。
  - **先看取消**：`await_suspend` 时若 stop 已被请求，一律 `UV_ECANCELED`，即使资源立即可用
    （与 sleep / tick 一致；值不被发送 / 不被取走 / 锁不被获取）。
  - 挂起中被取消：经 loop 投递，若仍在队列中则出队并以 `UV_ECANCELED` 恢复。
    **已被唤醒（出队）的等待者结果在唤醒时确定**，之后到达的取消不起作用。
  - `await_resume` 不再访问原语本身（Mutex 的 Guard 除外，此时调用方已持锁），
    因此等待者被唤醒后原语即可销毁。带着等待者销毁原语、带着锁销毁 Mutex 是契约违规。
- `Event{loop, initiallySet=false}`：`set()` 唤醒全部等待者并保持置位；`reset()`；`isSet()`；
  `wait() -> Result<void>`，已置位立即完成。
- `Mutex{loop}`：`lock() -> Result<void>`（之后手动 `unlock()`）、`scopedLock() -> Result<Mutex::Guard>`
  （只可移动，析构或 `unlock()` 时解锁）、`tryLock()`、`isLocked()`。
  `unlock()` 有等待者时把锁**直接移交**给队首（锁保持占用，`tryLock()` 无法插队），严格 FIFO。
- `Channel<T>{loop, capacity = Channel<T>::unbounded}`：`capacity == 0` 为会合通道。
  - `send(T) -> Result<void>`：有接收者等待则直接交给队首接收者；否则缓冲未满入缓冲；否则挂起。
    已关闭 → `UV_EPIPE`。
  - `recv() -> Result<T>`：先取缓冲（取走后把队首挂起发送者的值补进缓冲并唤醒它）；缓冲为空时
    直接从队首发送者取值（会合）；都没有且已关闭 → `UV_EOF`；否则挂起。
  - `close()`：幂等；挂起的接收者以 `UV_EOF`、挂起的发送者以 `UV_EPIPE` 唤醒（其值被丢弃）；
    已缓冲的值仍可取完，之后才是 `UV_EOF`。
  - `trySend(v)` / `tryRecv()`：不挂起；满 / 空时 `UV_EAGAIN`（trySend 失败不移动实参），
    关闭后分别为 `UV_EPIPE` / `UV_EOF`。另有 `size()`、`capacity()`、`isClosed()`。
- `whenAny(Task<T>...)` / `whenAny(std::vector<Task<T>>) -> WhenAnyResult<T>{index, value}`
  （`T = void` 时只有 `index`）。
  - 与 `co2::whenAll` 同构：awaiter 即操作状态，每分支一个手写续体帧，计数 N+1。内部 `stop_source`
    经 `stop_callback` 链到父 token。
  - 第一个完成（包括以异常结束）的分支 CAS 成为胜出者并 `request_stop()`；**等所有分支结束**后
    恢复等待者。全部分支按顺序启动（启动途中已胜出时，之后的分支看到 stop 已请求）。
  - 胜出者的异常在 `await_resume` 中重抛；落败者的结果与异常被丢弃。
  - 分支可以在别的线程上完成；等待者在最后完成的分支所在线程恢复（同 whenAll）。
  - 分支须同类型（`static_assert`）；空 vector 是契约违规。
- `timeout(loop, ms, Task<Result<T>>) -> Result<T>`：`whenAny(task, 计时分支)`。task 胜出 → 它的结果
  （含它自己的错误 / 异常）；计时分支胜出 → `UV_ETIMEDOUT`；父 token 取消 → 两边都结束，
  计时分支先完成时为 `UV_ECANCELED`。task 忽略取消时，超时后仍要等它结束。返回 awaitable
  （不是 Task）；计时分支是一个帧内放 `Sleep` 的小 Task。

#### 恢复策略（sync 与 timer 统一）

“谁的栈上运行被唤醒的协程”按唤醒来源决定：

- **就地恢复**：唤醒来自 libuv / loop 回调，栈上没有用户协程帧——I/O 与请求完成、定时器到期、
  经 `uv_async` 投递的取消节点（`CancelHook`）。这样少一次排队，且不会重入用户代码。
- **经 `loop.schedule()` 延后恢复**：唤醒来自用户代码——`Event::set`、`Mutex::unlock`、
  `Channel::send/recv/close`、`Timer::stop`、句柄 `close()` 中止挂起操作。唤醒方继续执行到
  它自己的挂起点，不会在 `set()` / `unlock()` 的栈上运行任意多个等待者（避免深递归、
  迭代中修改队列、以及“唤醒者以为还持有的状态已被等待者改掉”）。延后恢复的顺序就是唤醒顺序
  （本地队列 FIFO）。
- **组合子**（whenAll / whenAny / timeout）走 Task 续体（对称转移），不额外排队。

timer 已经遵循这一规则（§8 原待定问题据此关闭）。

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
- `TickAwaiter` 只持有控制块指针，应立即 `CO2_AWAIT(t.tick())`；若先取出 awaiter、跨过 `close()`
  再等待则悬垂（目前无检查）。
- ~~定时器回调路径内联恢复、stop()/close() 延后恢复是否要统一~~：已在 §4.5“恢复策略”中统一
  （按唤醒来源区分，timer 现有行为即符合）。
- sync / whenAny 额外依赖 co2 `detail`：`ForwardStop`、`LateInit`、`HandleAccess::header`、
  `TaskAccess::arm/hasError/rethrowError/takeResult`（与 co2::whenAll 的实现方式相同）。
- `timeout` 只接受 `Task<Result<T>>`；普通 `Task<T>` 需调用方自行包装（是否提供适配重载待定）。
- whenAny 只支持同类型分支；异构分支需要 C++17 `variant` 或用户包装，暂不提供。
- sync 原语不支持跨线程使用；跨线程通信目前只能经 `scheduleOn(loop)` 回到 loop 线程再操作。
- 计时测试有毫秒级容差（如 20ms sleep 断言 ≥19ms，libuv 时钟为毫秒截断）。
- 尚未提供 install / export 规则（目前只支持 `add_subdirectory` / FetchContent 方式使用）。

## 9. 与原设计的偏差（实现 core 时记录）

- §4.1 原写“`run()` 完成后关闭并排空剩余句柄”：与 §7 用户持有 Loop 冲突，改为 `run()` 只驱动；
  句柄清理交给用户，`~Loop` 检查。
- §4.1 本地队列明确使用 `uv_idle`，远程队列使用 `uv_async`；远程队列为互斥量保护的 vector
  而非无锁 MPSC（足够简单，且锁内 `uv_async_send` 解决了 Loop 销毁竞态）。
- §4.1 `ReqAwaiter` 模板参数改为 `<Derived, Req, T>`（CRTP），以便每种请求自定义发起、完成与取消。
- Loop 额外绑定构造线程并限制每线程一个，以便 `isLoopThread()` 检查与 `Loop::current()`。


### 实现 timer 时记录

- §4.2 扩展：`close()` 中止挂起操作（`UV_ECANCELED`）；`close()` 后所有者立即为空；未等待的 close 仍关闭。
- §4.3 取消机制抽成 `detail::CancelHook`，供请求与句柄共用。
- §4.4 时长用 `std::chrono::milliseconds`；`tick()` 返回到期次数（`Result<uint64_t>`）而非 `Result<void>`；
  stop_token 取消 tick 不停止定时器；启动前 `uv_update_time`。
- `sleep()` 基于 `ReqAwaiter`（内嵌 `uv_timer_t`，恢复前关闭），而非借用 `Timer`。

### 实现 sync 时记录

- §4.5 whenAny 只支持同类型分支（`WhenAnyResult<T>{index, value}`），原写“`whenAny(a, b, ...)`”未限定类型。
- §4.5 whenAny 的“完成”包括异常：抛异常的分支也能胜出，其异常被重抛；落败者的异常被丢弃。
  落败者不必以 ECANCELED 结束——只要求结束（结果被丢弃）。
- `Channel::send` 在关闭后返回 `UV_EPIPE`（原设计只规定了 recv 的 `UV_EOF`）；`capacity == 0` 为会合通道；
  增加 `trySend` / `tryRecv`（`UV_EAGAIN`）。
- 统一“先看取消”规则：已请求停止时 wait / lock / send / recv 一律 `UV_ECANCELED`，即使可以立即完成。
- 等待节点内嵌在 awaiter 中，`CancelHook` 仅在以可停止 token 挂起时分配。
- Mutex 解锁时直接移交（不可插队），并提供 `scopedLock()` 返回 `Guard`。
- `timeout` 只接受 `Task<Result<T>>`，返回 awaitable 而非 Task。
