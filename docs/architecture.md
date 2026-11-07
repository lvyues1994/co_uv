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
- 取消统一表现为 `UV_ECANCELED`（`Errc::canceled`）：只用于 stop_token 取消。
- 操作因句柄被 `close()` / `stop()` 或 Loop 析构而结束 → uvco 专有的 `Errc::aborted`
  （`UVCO_EABORTED`，取值远离 libuv 的错误码范围；`strerror` / `errorName` / `uv_category`
  单独处理它）。调用者由此区分“我取消了”与“句柄没了”。
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
  - **保活**（借鉴 kotatsu 的 relay）：`uv_async` 默认 unref，只有跨线程工作在路上时才 ref。
    计数 `keepAlive_`（互斥量保护）由 `run()`（根 Task 未完成期间）和用户的 `Loop::KeepAlive`
    （`loop.keepAlive()`，loop 线程取得，只可移动，任意线程销毁）构成，0→1 时 `uv_ref`。
    释放在锁内递减**并在锁内** `uv_async_send`；只有 async 回调末尾在锁内复查“计数为零且远程
    队列为空”后才 `uv_unref`。因此 loop 不可能在最后一次唤醒发出之前判定空闲、返回并被销毁
    （tests/keepalive_release_race_test.cpp 用 `--wrap=uv_async_send` 复现这一窗口）。
  - `~Loop` 兜底（借鉴 kotatsu `~event_loop`）：句柄控制块登记在 Loop 上（`detail::LoopResident`）。
    析构时循环：对仍打开的 uvco 句柄 `closeInternally()`（挂起操作以 `Errc::aborted` 结束）、
    `uv_walk` 关闭其他句柄（close 回调把 `data` 置空）、`uv_run(ONCE)`，直到 loop 不再存活；
    其间被恢复的协程新开的句柄也会在下一轮被关闭。然后关闭内部 async / idle，`uv_loop_close`
    必须成功。仍被用户持有的控制块成为“孤儿”：之后只能 `close()` / 析构（直接释放），其他操作
    契约违规。限制：析构时挂起中的 `Sleep`（内嵌 `uv_timer_t` 的请求）会被当作外部句柄关闭，
    其等待者永远不会恢复——销毁 Loop 前应让所有 Task 结束。
  - 首次构造 Loop 时（每进程一次）若 `SIGPIPE` 仍为默认动作则忽略它：写已关闭的连接返回
    `EPIPE` 而不是杀死进程；用户设置过的动作不变。
- `run(loop, task, token = {}) -> T`：loop 版 `syncWait`，启动 Task 并 `uv_run` 直到 Task 完成，
  然后返回值或重抛异常。可选的 `token` 作为整棵任务树的根 stop_token。
  - Task 若在别的线程上完成，完成通知先经 `schedule()` 回到 loop 线程，再停止 `uv_run`。
  - **不**关闭、不排空用户句柄（与 §7 “用户持有 Loop”一致，同一 loop 可多次 `run()`）。
    剩余句柄由用户负责，`~Loop` 兜底关闭（见上）。
- `drain(loop)`：运行直到 loop 无事可做（无活动句柄 / 请求、无排队协程、无存活 KeepAlive），
  例如 `co2::spawn(loop, task)` 之后。把工作交给 loop 看不见的线程再投递回来的代码必须持有
  `KeepAlive`，否则 `drain()` 可能在投递到达前返回。
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
  - 扩展：`close()` 时若有挂起操作，以 `Errc::aborted` 中止它们（经 `loop.schedule` 延后恢复），
    而不是契约违规——显式 close 是合法的“取消一切”手段；只有直接析构才算违规。
  - 句柄可以在 Loop 析构之前一刻销毁；也可以比 Loop 活得久：`~Loop` 关闭它并把控制块变为孤儿
    （§4.1），孤儿句柄的 `close()` / 析构只是释放控制块，其他操作是契约违规
    （tests/orphan_handle_use_contract_test.cpp）。
  - 所有权与关闭分离（stream 引入）：`HandleBlock::release(closeWaiter)` 由所有者调用恰好一次
    （close / 析构 / CloseAwaiter），`closeInternally()` 可由库内部提前发起关闭（如取消 connect），
    此时所有者仍持有控制块（`isOpen()==true`），后续操作返回 `UV_EBADF`，`close()` 仍可等待。
    控制块释放条件为“close 回调已执行 且 无在途操作 且 所有者已释放”。

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
  - stop_token 取消 tick → `UV_ECANCELED`，**定时器继续运行**；`stop()` / `close()` / Loop 析构使
    挂起的 tick 以 `Errc::aborted` 结束。

### 4.5 sync（loop 内单线程原语）

- `Event`：set / wait。
- `Mutex`：协程互斥锁，FIFO 唤醒。
- `Channel<T>`：有界/无界，`send` / `recv`，`close()` 后 `recv` 返回 `UV_EOF` 类错误。
- `whenAny(a, b, ...)`：分支是任意 awaitable（可异构）；第一个**成功**的分支胜出，
  用内部 `stop_source` 取消其余分支，**等待所有已启动分支结束后**才返回；返回胜出下标与值。
  分支以错误结束则比赛以该错误结束（规则参照 kotatsu `when_any`）。
- `timeout(loop, ms, op)`：基于 `whenAny(op, sleep)`，超时返回 `UV_ETIMEDOUT`；
  `runUntil(op, stops...)`：任一 stop 先成功 → op 被取消，`UV_ECANCELED`。

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
- `whenAny(a, b, ...) -> Result<WhenAnyResult<S...>>`（`sync/when_any.hpp`）
  - 分支：任意 awaitable——`co2::Task<V>` 原样使用，其他 awaitable（`sleep`、`Channel::recv`、
    句柄操作、`Event::wait`…）包成内部 `Task<V>`（每个一次帧分配）。`V` 是 `await_resume()` 的
    decay 类型（引用结果被拷贝）。分支按值接收（Task 须 `std::move`）。
  - 成功值 `S`：`V = Result<U>` → `U`（`Result<void>` → `void`）；普通 `V` → `V`（永远算成功）。
  - `WhenAnyResult<S...>`：`index()`、成员 `get<I>()` 与自由函数 `uvco::get<I>(r)`（前置条件
    `index() == I`，否则契约违规；void 分支的 `get` 返回 void）。C++14 无 `variant`：对齐的未初始化
    存储 + 下标，移动 / 析构经下标查表分发。只可移动；默认构造为空（仅用作协程帧局部变量）。
  - 结束分类：成功 / ECANCELED（取消，不算错误）/ 其他 Result 错误 / 异常。**第一个结束的分支**
    （任何一类）CAS 决出比赛并 `request_stop()` 其余分支。
  - 结果优先级：① 任一分支的异常（按结束顺序第一个）重抛；② 任一分支的第一个非取消错误
    （**即使发生在胜者产生之后、落败者被取消途中**，同 kotatsu“取消途中也记录第一个错误”）；
    ③ 决出者：成功 → 它的值，ECANCELED → `UV_ECANCELED`。被取消落败者的值被丢弃。
  - 先看取消：`await_suspend` 时父 token 已请求停止 → 不启动任何分支，`UV_ECANCELED`。
    比赛中途的父取消经 `stop_callback` 转发到内部 `stop_source`。
  - 分支按参数顺序启动；**比赛已决出后尚未启动的分支不再启动**（直接随 awaiter 销毁）。
    因此同步完成的分支之后的分支不会运行。
  - 实现：awaiter 即操作状态（与 `co2::whenAll` 同构），每分支一个手写续体帧；计数 N+1，
    启动方放下自己那份与未启动分支的份额。分支结果在续体中取出并分类（异常留在 Task 中）。
    分支可以在别的线程上完成；等待者在最后结束的分支所在线程恢复。
- `whenAny(std::vector<A>) -> Result<WhenAnyRangeResult<S>>{index, value}`（`S = void` 时只有 `index`）：
  同类型分支，规则同上；空 vector 是契约违规（tests/sync_when_any_empty_contract_test.cpp）。
- `timeout(loop, ms, op) -> Result<S>`（`sync/timeout.hpp`）= `whenAny(op, sleep(loop, ms))`：
  op 先成功 → 它的值；计时器先到 → `UV_ETIMEDOUT`；op 的错误 / 异常按上面的优先级；父取消 →
  `UV_ECANCELED`。op 先启动：op 同步完成时计时器不启动。op 忽略取消时超时后仍要等它结束。
  op 可以是任意 awaitable（`Task<int>` → `Result<int>`，`sleep` → `Result<void>`，`ch.recv()` …）。
- `runUntil(op, stops...) -> Result<S>` = `whenAny(stops..., op)`：任一 stop 先成功结束 → op 被取消，
  `UV_ECANCELED`（stop 的值被丢弃，stop 的非取消错误照常传出）。stops 先启动：已满足的 stop
  让 op 根本不启动（同 kotatsu `run_until`）。
- `timeout` / `runUntil` 返回 awaitable（不是 Task），结果映射不额外占帧。

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

头文件：`uvco/stream.hpp`（`stream/buffer.hpp`、`address.hpp`、`stream.hpp`、`tcp.hpp`、`pipe.hpp`）。

- 缓冲区：`MutableBuffer{char*, size}` / `ConstBuffer{char const*, size}`，`buffer(...)` 适配指针+长度、
  `std::string`、`std::vector<char>`、`char[N]`。**缓冲区由调用方持有，必须活到 await 结束**；
  右值 string / vector 的重载被删除以避免悬垂。
- `SocketAddress`：`ipv4(ip, port)` / `ipv6` / `parse(ip, port)`（含 `:` 视为 IPv6）→ `Result<SocketAddress>`，
  `fromRaw(sockaddr const*)`、`family()`、`port()`、`ip()`、`toString()`（`ip:port` / `[ip]:port`）。不做 DNS。
- 公共流接口 `detail::StreamHandle<Block, Self>`（`Tcp`、`Pipe` 共用，后续 `Tty`）：
  - `read(MutableBuffer) -> Result<size_t>`：拉模式。仅在有读者挂起时 `uv_read_start`，每次回调后
    立即 `uv_read_stop`，无读者时数据留在内核缓冲区形成背压。`alloc_cb` 直接交出读者的缓冲区（零拷贝），
    因此**没有溢出缓冲**；无读者时给出 0 长度缓冲 → `UV_ENOBUFS` → 停止读取（防御性）。
    EOF 返回 `UV_EOF`；EOF 与读错误是**粘性**的，之后的 read 立即返回同一错误。
    同一流上并发两个 read → 契约违规。空缓冲区 → 契约违规。
  - `write(ConstBuffer)` / `write(std::vector<ConstBuffer>) -> Result<void>`：先 `uv_try_write` 快速路径，
    未写完的部分（裁剪后）交给 `uv_write`（请求对象按需堆分配）。完成 = 全部数据交给内核。
    并发写按调用顺序排队（libuv 写队列保证顺序；队列非空时 `uv_try_write` 返回 EAGAIN）。
    空缓冲区被跳过；全部为空时立即成功。
  - `shutdown() -> Result<void>`：等待已排队写完成后关闭写端（半关闭，仍可读）。
  - `listen(backlog = 128) -> Result<void>`、`accept() -> Result<Self>`：拉模式 accept。
    `connection_cb` 只在有 accept 等待者时取连接；无等待者时 libuv 自身保留一个已接受 fd 并停止监听读事件，
    其余连接留在内核 backlog（背压）。未 listen 时 accept → `UV_EINVAL`；并发两个 accept → 契约违规。
  - `writeQueueSize()`、`isReadable()`、`isWritable()`、`close()`、`isOpen()`。
- `Tcp`：`Tcp()`（空句柄，可作协程帧局部变量、接收 accept 结果）、`explicit Tcp(Loop&)`、
  `bind(SocketAddress, flags = 0)`、`connect(SocketAddress)`、`setNoDelay(bool)`、
  `setKeepAlive(bool, seconds delay = 60s)`、`localAddress()` / `peerAddress() -> Result<SocketAddress>`、`raw()`。
- `Pipe`：`Pipe()`、`explicit Pipe(Loop&)`、`bind(path)`、`connect(path)`、
  `localName()` / `peerName() -> Result<std::string>`、`raw()`。不支持 IPC（传递句柄）。
- 取消（stop_token，统一“先看取消”规则：已请求停止 → `UV_ECANCELED`，不发起操作）：
  - read：`uv_read_stop` 并以 `UV_ECANCELED` 完成；流保持可用，未读数据不丢失。
  - accept：以 `UV_ECANCELED` 完成；监听继续，之后可再 accept。
  - connect：libuv 无法取消进行中的 connect，因此**取消 = 关闭流**（`closeInternally()`），connect 以
    `UV_ECANCELED` 完成（同一流上其他挂起操作以 `Errc::aborted` 结束）；之后该流上的操作返回
    `UV_EBADF`，仍需（也可以）`close()` / 析构。
  - write / shutdown：发起后不可取消（只在发起前检查停止）；`timeout(write)` 不能截断已排队的写，
    要中止只能 `close()`。
  - `close()`（或 Loop 析构）中止所有挂起的 read / write / accept / connect / shutdown，均以
    `Errc::aborted` 结束（read / accept 延后恢复；write / shutdown / connect 由 libuv 回调的
    `ECANCELED` 映射而来——不可取消的 write / shutdown 的 `ECANCELED` 只可能来自 close，connect 记录
    是否由 stop_token 发起取消）。
    被中止或失败的写不报告已写字节数。
- 在已（内部）关闭的流上 read / write / accept / connect / shutdown → `UV_EBADF`。
- 恢复策略同 §4.5：libuv 回调与取消节点中内联恢复；close 中止延后恢复。
- 未实现：`chunks() -> co2::AsyncGenerator<Chunk>`（可在 read 之上由用户构造，暂不提供）。

### 4.7 udp

- `Udp::bind`、`connect` / `disconnect`、`send(addr, buf)` / `send(buf)`（已连接）`-> Result<void>`、
  `recv(buf) -> Result<Datagram>`（`size`、来源地址 `sender`、`truncated`）、`localAddress` /
  `peerAddress`、`setBroadcast` / `setTtl`。实现在 `include/uvco/udp/udp.hpp`。
- 读取同 stream 的拉模式：只有 `recv()` 挂起时才接收，`onAlloc` 直接交出等待者的缓冲区，
  每次 `recv()` 恰好一个数据报；本轮已交付后 `onAlloc` 交出空缓冲区，libuv 以 `UV_ENOBUFS` 回调，
  数据留在内核里。
- **不在 recv 回调里停止或恢复**（借鉴 kotatsu `src/async/io/udp.cpp`：Linux 上 libuv 在 EPOLLERR
  之后读错误队列时断言套接字仍在接收）：完成经 `loop.schedule` 推迟到本轮回调之后恢复；
  “没有等待者就 `uv_udp_recv_stop`”也是一个推迟任务（控制块内的手写协程帧，排队期间以
  `holdBegin()` 保活控制块，不算用户的挂起操作）。被恢复的协程立刻再次 `recv()` 时推迟的停止
  看到新的等待者，什么也不做，于是连续接收不会反复 start / stop。
- **没有等待者时到达的数据报不排队**：留在内核的套接字缓冲区（满了由内核丢弃），与 TCP 读的
  背压一致，零拷贝、无额外分配。kotatsu 在用户态排一个最多 64 个的队列并复制成 `std::string`；
  我们不这样做，需要更深缓冲的调用者可以加大 `SO_RCVBUF` 或保持一个 `recv()` 常挂起。
  没有等待者时 libuv 报告的错误保存一个，交给下一次 `recv()`。
- 缓冲区比数据报小：多余部分被丢弃（`UV_UDP_PARTIAL` → `truncated`）。空数据报正常收发
  （`size == 0`；libuv 的“0 字节、无地址”是“已读空”，被忽略）。
- send：快路径 `uv_udp_try_send`，`EAGAIN` 时才分配 `uv_udp_send_t`；目标地址按值复制
  （`sockaddr_in6`），awaiter 放得进 co2 的内联槽。发起后不可取消；超过 65535 字节 → `EMSGSIZE`。
- 取消 `recv` → `Errc::canceled`（先停止规则同 stream）；`close()` / Loop 析构 → 挂起的 recv / send
  以 `Errc::aborted` 结束。同一套接字上同时两个 `recv()` 是契约违规。
- 已连接套接字收到 ICMP 端口不可达时 `recv()` 返回 `Errc::connection_refused`，套接字仍可用。

### 4.8 fs

头文件 `uvco/fs.hpp`，命名空间 `uvco::fs`。全部基于 `detail::ReqAwaiter<…, uv_fs_t, …>`。

- **通用 awaiter**：每个操作是 `fs::XxxOp = fs::FsOp<detail::XxxSpec>`。Spec 按值保存参数
  （路径为 `std::string`，因为 Task 是惰性的、`CO2_AWAIT` 语句里的临时对象在挂起前就已销毁），
  提供 `submit()`（发起 `uv_fs_xxx`）、`project()`（把成功的请求转换为值）与可选的 `release()`。
  新增一个操作只需要一个十来行的 Spec 加一行别名 / 便捷函数（借鉴 kotatsu 的 `fs_call`）。
- **uv_fs_req_cleanup 总会执行**：libuv 回调里先 `project()`，再 `uv_fs_req_cleanup`，最后恢复等待者；
  发起失败时也立即清理。
- **两种等待方式**（§7）：
  - 帧局部：`fs::StatOp op{loop, path};` 写在 `CO2_BEG` 局部列表里，`CO2_AWAIT_SET(r, op);`——
    零分配，可重复等待，两次等待之间经 `op.args()` 修改参数（在途时调用是契约违规）；
  - 右值：`CO2_AWAIT_SET(r, fs::stat(loop, path));`——`uv_fs_t` 超出 co2 内联槽，一次堆分配。
- **File**：只可移动的 fd 所有者。`fs::open(loop, path, flags, mode = 0644)` 返回 `Result<File>`；
  `read(buf, offset = -1)` / `write(buf, offset = -1)`（-1 为当前位置）返回字节数，
  `sync()`、`truncate(size)`、`stat()`、`close()`（File 立即变空，awaiter 持有 fd），
  `release()` 交出 fd，`reset()` 同步关闭。**析构时若仍打开则同步关闭**（`uv_fs_close` 无回调，
  阻塞调用线程一次 `close(2)`；不依赖 Loop 仍然存活）。File 的操作按值捕获 fd：在操作在途时
  关闭 / 析构 / 移走 File 是调用者的错误（与对同一 fd 调用 `close(2)` 一样，fd 可能被重用）。
- **自由函数**：`stat`、`lstat`、`unlink`、`mkdir(mode = 0777)`、`rmdir`、`rename`、
  `copyfile(flags = 0)`、`mkdtemp`（返回路径）、`mkstemp`（返回 `TempFile{File, path}`）、
  `readlink`、`realpath`、`scandir`（返回 `std::vector<DirEntry>`，不含 `.`/`..`）、
  `sendfile(out, in, inOffset, length)`；便捷协程 `readFile(loop, path)` → `Result<std::string>`、
  `writeFile(loop, path, data, flags = WRONLY|CREAT|TRUNC, mode = 0644)` → `Result<void>`，
  内部用帧局部 awaiter，除 Task 帧与数据外零分配。
- **取消**：通过 `uv_cancel`，尽力而为——只能撤下还没被线程池取走的请求。Linux 上 libuv（≥1.45）
  可能把部分 fs 请求（open、close、read、write、fsync、stat……）交给 io_uring，此时 `uv_cancel` 返回
  `EBUSY`，请求自然完成。
- **迟到的取消**（借鉴 kotatsu 的 Release 钩子）：取消已请求、但请求仍然成功完成时，
  产生资源的操作在回调里同步释放资源并报告 `Errc::canceled`——`open` 关闭 fd，`mkstemp` 关闭 fd
  并删除文件，`mkdtemp` 删除目录。不产生资源的操作照常报告真实结果（副作用已经发生，例如
  unlink；read 读到的数据也照常返回）。
- **Loop 析构**：fs 请求不是句柄，`~Loop` 的 `UV_RUN_ONCE` 循环会把在途请求驱动到完成，
  等待者照常拿到结果——没有 `Errc::aborted` 路径（被阻塞的请求，例如打开没有写端的 FIFO，
  会让 `~Loop` 一直等下去）。
- 测试在 io_uring 开 / 关两种模式下各跑一遍（ctest 的 `UV_USE_IO_URING=1` / `=0`）。

### 4.9 process（v0.1 之后的扩展）

`uvco/process.hpp` 提供 `uv_spawn` 包装，不使用 shell 隐式解释参数。

- `spawn(loop, ProcessOptions)` 同步返回 `Result<Process>`；`Process` 只可移动，控制块继承
  `HandleBlock` 并持有 `uv_process_t`。启动失败也异步关闭 libuv 已初始化的 handle。
- `ProcessOptions`：`file`、`args`（不包含 argv[0]，库自动用 file 补齐）、`env`、`inheritEnv`、
  `cwd`、`stdio`、`detached`、`setUid/uid`、`setGid/gid`。空 env 默认继承父环境；
  `inheritEnv=false` 且空 env 表示空环境；非空 env 为完整替换而非增量覆盖。
- stdio 默认忽略三个标准描述符；`ProcessStdio::ignore()`、`inherit(fd)`、`inherit(Pipe&)`、
  `createPipe(Pipe&, childReads, childWrites)`。方向按子进程视角：stdin 是 childReads，stdout
  是 childWrites。调用者先创建同一 Loop 上尚未打开的 Pipe，再把它传给 createPipe；spawn 后它就是
  父端 stream，继续使用现有 read/write/shutdown/close。指针仅在 spawn 调用期间使用。
- `pid()` 返回 pid；`kill(signum)` 返回 Result<void>，仅显式调用才发送信号。
- `wait()` → `Result<ExitStatus{exit_status, term_signal}>`。退出回调缓存结果，退出后再等立即完成，
  可以重复等待；同一时刻只能有一个 wait（含完成但尚未恢复的阶段，违反则契约失败）。
  libuv 退出回调内联恢复；取消和 close 经 Loop 调度恢复。
- stop_token 只取消等待（canceled），不杀进程；预先停止优先于已缓存的退出结果。
  取消后可再次 wait。close/Loop 析构中止等待为 aborted。
- 析构/close 不杀子进程。正常路径应先 wait 再 close；运行中关闭 handle 会停止 libuv 的退出监视，
  POSIX 调用者需自行处理之后的 waitpid/reaping，可能留下僵尸，库不提供后台 reaper。
  析构时有挂起 wait 仍沿用通用 Handle 契约：先取消并恢复，或显式 close 中止；不能直接析构。
- detached 只设置 UV_PROCESS_DETACHED，不自动 unref：持有的运行中 Process 仍保活 Loop。
  Loop 析构关闭 handle 而不等待子进程退出；控制块成为孤儿，之后只可 close/析构。
- 线程约束、关闭 awaiter 和控制块保活协议均复用 §4.2。WaitAwaiter 自带 CancelHook，体积超出
  co2 内联槽，直接 await 的时候有一次 awaiter 堆分配（本模块不承诺零分配）。

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
  带着挂起请求析构 awaiter、带着存活的 `KeepAlive` 析构 Loop、在 Loop 析构后使用孤儿句柄——
  都是契约违规（走 co2 的契约处理器），不抛异常。（带着打开的句柄析构 Loop 已不再违规，见 §4.1。）

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
- ~~`timeout` 只接受 `Task<Result<T>>`~~、~~whenAny 只支持同类型分支~~：已由 whenAny 重做解决
  （任意 awaitable、异构分支，见 §4.5）。另依赖 co2 `detail::AwaiterOf`、`ResultStorage`。
- whenAny 在分支续体中移动分支结果：结果类型的移动构造抛异常 → `std::terminate`（续体无处报告）。
- whenAny 与 `co2::whenAll` 各自实现；kotatsu 用一个 `when_op` 同时实现 all / any，可在 co2 侧统一。
- sync 原语不支持跨线程使用；跨线程通信目前只能经 `scheduleOn(loop)` 回到 loop 线程再操作。
- 计时测试有毫秒级容差（如 20ms sleep 断言 ≥18ms，libuv 时钟为毫秒截断，ASan 下偶见 19ms）。
- stream：无 `Tty`；Pipe 不支持 IPC（传递句柄）与 Linux 抽象套接字（`uv_pipe_bind2` 需 libuv ≥1.46，
  而最低版本为 1.40）；不能在不关闭流的情况下取消写；connect 取消会关闭流；Windows 行为未测试
  （命名管道、`uv_try_write` 语义不同）。
- stream 依赖 libuv 的若干实现行为（已在 1.48 源码核对）：无 accept 时保留一个已接受 fd 并停止监听、
  `READING` 标志清除后读循环立即停止、关闭时以 `UV_ECANCELED` 回调 connect / write。
- `~Loop` 无法恢复挂起中的 `Sleep`（它的 `uv_timer_t` 被当作外部句柄关闭）；可考虑让 `Sleep`
  也登记为 LoopResident。
- 远程投递（`schedule` / `post`）的线程若既不在 `run()` 期间、也不持有 `KeepAlive`，与 `drain()` /
  `~Loop` 竞争（`~Loop` 发现未处理的远程工作即契约违规）。
- 保活释放竞争测试依赖 GNU ld / lld 的 `--wrap`，只在 Linux 上构建。
- 尚未提供 install / export 规则（目前只支持 `add_subdirectory` / FetchContent 方式使用）。

## 9. 与原设计的偏差（实现 core 时记录）

- §4.1 原写“`run()` 完成后关闭并排空剩余句柄”：与 §7 用户持有 Loop 冲突，改为 `run()` 只驱动；
  句柄清理交给用户，`~Loop` 检查。（后由“借鉴 kotatsu 改进 core 时记录”改为 `~Loop` 兜底关闭。）
- §4.1 本地队列明确使用 `uv_idle`，远程队列使用 `uv_async`；远程队列为互斥量保护的 vector
  而非无锁 MPSC（足够简单，且锁内 `uv_async_send` 解决了 Loop 销毁竞态）。
- §4.1 `ReqAwaiter` 模板参数改为 `<Derived, Req, T>`（CRTP），以便每种请求自定义发起、完成与取消。
- Loop 额外绑定构造线程并限制每线程一个，以便 `isLoopThread()` 检查与 `Loop::current()`。


### 实现 timer 时记录

- §4.2 扩展：`close()` 中止挂起操作（`UV_ECANCELED`，后改为 `Errc::aborted`）；`close()` 后所有者立即为空；未等待的 close 仍关闭。
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
- 以上前两条与最后一条中关于 whenAny / timeout 的语义已被“重做 whenAny 时记录”取代。

### 实现 stream 时记录

- §4.6 原写“多余数据进入小缓冲”：改为**无溢出缓冲**。每次读回调后立即 `uv_read_stop`，`alloc_cb` 只在有读者
  时被调用并直接交出读者缓冲区（零拷贝）；无读者时给 0 长度缓冲，数据留在内核。
- EOF / 读错误粘性；并发 read、并发 accept 为契约违规（原设计未规定）。
- accept 为拉模式，背压依赖 libuv 的“保留一个已接受 fd”行为；预先创建的对端控制块在失败时复用。
- connect 无法真正取消：取消即关闭流（§4.2 增加 `release()` / `closeInternally()` / 所有者标志）。
- write / shutdown 发起后不可取消；`close()` 以 `UV_ECANCELED`（后改为 `Errc::aborted`）中止它们。
- `Tcp` / `Pipe` 提供空的默认构造（便于作协程帧局部变量与接收 accept 结果）。
- `connect` 接受 `SocketAddress`（而非原写的 `sockaddr` / `ip4_addr` / `ip6_addr` 三种形式）。
- 新增错误码 `Errc::bad_descriptor`（`UV_EBADF`）、`Errc::not_connected`（`UV_ENOTCONN`）。
- `chunks()` 生成器未实现。

### 重做 whenAny 时记录

参照 kotatsu（clice-io/kotatsu `include/kota/async/runtime/when.h`）重做：

- 变参 `whenAny` 接受异构、任意 awaitable，返回 `Result<WhenAnyResult<S...>>`（`index()` / `get<I>()`）；
  vector 重载仍为同类型，返回 `Result<WhenAnyRangeResult<S>>{index, value}`。原 `WhenAnyResult<T>{index, value}` 删除。
- 胜出规则由“第一个完成者（含异常）胜出”改为“第一个**成功**者胜出”：错误结束比赛并传出；
  异常优先级最高，**落败者的异常不再被丢弃**；落败者在取消途中的非取消错误也会传出。
- 总是包一层 `Result`（uvco 只有一种错误类型 `Errc`），因此异构分支的错误可以统一传出。
- 父 token 已停止时不启动分支（先看取消，与其他原语一致）；比赛决出后不再启动后续分支
  （原为“之后的分支照常启动并看到 stop”），同 kotatsu“立即结束的 stop 阻止任务启动”。
- `timeout` 泛化为任意 awaitable，op 先于计时器启动；新增 `runUntil(op, stops...)`。

### 借鉴 kotatsu 改进 core 时记录

参照 kotatsu（clice-io/kotatsu `src/async/runtime/loop.cpp`、`tests/async/relay_release`）：

- §4.1 `uv_async` 由“`run()` 期间 ref”改为“保活计数非零时 ref”：新增 `Loop::KeepAlive` 与
  `drain(loop)`；释放在锁内递减并唤醒，async 回调末尾锁内复查后才 unref。审计结论：旧实现
  在 `run()` 期间始终 ref，因此没有 kotatsu 修复过的那个竞态；新协议照搬 kotatsu 的“锁内释放 +
  回调末尾复查”，并加了 `--wrap=uv_async_send` 回归测试（把锁外发送的变体改回来，测试即失败）。
- §4.1 / §4.2 `~Loop` 不再在 `EBUSY` 时契约违规：关闭登记的 uvco 句柄与外部句柄，`UV_RUN_ONCE`
  直到 loop 不再存活，剩余控制块成为孤儿。
- §4.1 首次构造 Loop 时忽略 `SIGPIPE`（仍为默认动作时）。
- §2 新增 `Errc::aborted`：close / stop / Loop 析构中止的操作不再报告 `UV_ECANCELED`；stop_token
  取消仍为 `Errc::canceled`。sync 原语没有“被关闭中止”的路径（Channel 关闭仍为 `EOF` / `EPIPE`）。

### 实现 udp（第 5 步）时记录

- §4.7 原写“有等待者时 `uv_udp_recv_start`，否则 stop”：停止不在 recv 回调里做，推迟到下一轮
  （见 §4.7）；完成也推迟恢复，而 stream 的 read 在回调里内联恢复。
- 明确了“没有等待者时的数据报”：不排队，留在内核里（kotatsu 排 64 个）。
- 为推迟任务在 `HandleBlock` 增加 `holdBegin()` / `holdEnd()`：保活控制块但不计入
  `hasPendingOps()`，因此句柄在推迟的停止排队时仍可正常析构。

### 实现 fs（第 6 步）时记录

参照 kotatsu（clice-io/kotatsu `src/async/io/fs.cpp`）：

- §4.8 原文只列了 `File::open/read/write/close`：`open` 改为自由函数 `fs::open` 返回 `Result<File>`
  （与 `mkstemp` 一致）；增加 `File::sync/truncate/stat/release/reset` 与上文的自由函数；
  选择 `scandir`（一次返回全部条目）而非 `opendir/readdir`。
- C++14 没有 `auto` 模板参数，kotatsu 的 `fs_call<Project, Release>` 改写为 Spec 结构体 +
  `FsOp<Spec>`；有 `release` 成员即视为产生资源。
- 迟到的取消只对产生资源的操作报告 `Errc::canceled`；kotatsu 对所有操作都报告取消。
  这里保留真实结果，因为副作用（unlink、write……）已经发生，报告取消会误导调用者。
- 为 `FsOp::args()` 在 `ReqAwaiter` 增加受保护的 `inFlight()`。
- File 析构选择同步关闭（而非借助 Loop 的即发即忘）：不需要堆上的请求体，Loop 先析构也没有问题。
- File 的操作不跟踪在途计数（按值捕获 fd），“在途时关闭”只写进文档，不做契约检查。
- 测试：io_uring 下 `openat` 打开 FIFO 不阻塞（内核先以非阻塞方式打开），因此迟到取消的
  确定性用例只在 `UV_USE_IO_URING=0` 那一遍走到 release 路径；io_uring 那一遍只检查不泄漏，
  另用“阻塞在空 FIFO 上的 read 被取消后照常完成”覆盖 `EBUSY` 路径。TSan 构建不能替换全局
  `operator new`，零分配断言在 TSan 下跳过。


### 实现 process 时记录

- process 是 v0.1 完成后的新增模块，不改变原 v0.1 范围表；§7 的用户持有 Loop、通用句柄控制块
  和契约策略继续适用。不是 ReqAwaiter：uv_spawn 是同步启动 + handle 退出回调，而非单次请求。
- 参考本地 kotatsu `src/async/io/process.cpp`，其实现使用 sys::Child、pidfd/kqueue/平台 Watch 和
  orphan reaper；这里按批准设计保持纯 uv_spawn，借鉴缓存退出结果/单等待槽，不引入自建 reaper。
- pipe 采用调用者提供的 Pipe，而不是 Process 内藏三个流：所有权明确，spawn 失败后的 Pipe
  仍由调用者 close/析构。没有 communicate/capture 等缓冲便捷层。
- stdio 方向及互斥类型、字符串内嵌 NUL 等参数先校验，避免部分 libuv 参数错误触发断言。
- 测试依赖 Linux 的 /bin/sh、echo、cat、true、false、sleep、/proc/self/fd、waitpid。
  uid/gid 字段按 libuv 转发，尚未在权限切换环境或 Windows/macOS 验证。
- 四套构建通过；process 测试在 ASan/UBSan 额外连续 20 遍、TSan 全套 20 遍通过。
  ASan 全套并行重复曾遇旧 sync_tests.cpp:980 的 elapsed >= 9 && elapsed < 1000 计时断言失败，
  无 sanitizer 报告；未改无关测试，全套串行重复 10 遍通过。
