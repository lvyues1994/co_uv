# uvco 架构实现历程（历史记录）

本文是 `architecture.md` 重构前的完整记录：实现过程中的设计偏差、审查修订与验证笔记。
当前架构说明见 [architecture.md](architecture.md)。下文标题层级保持原样，便于对照历史提交。

---

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
    `CO2_AWAIT(op)`。co2 0be5ab2 起对左值就地等待，帧里只记指针，零堆分配，且 awaiter
    可重复等待。直接 await 一个临时 awaiter 也可以，但大请求会超出内联槽，产生一次堆分配。
    因此 `find_package(co2)` 必须通过 `cmake/co2_capability.cpp` 的就地左值探测；更早的
    co2 会把左值拷进槽，无法等待不可复制的帧局部 `ReqAwaiter`。

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
- **uv_fs_req_cleanup 总会执行**：投影成功/抛异常后都 cleanup，最后恢复等待者；发起失败时也立即清理。
  投影 bad_alloc 返回 no_memory，其余异常缓存后在 await_resume 重抛，不穿越 libuv 回调边界。
  资源型 Spec 必须在所有可抛步骤完成后才转移所有权（mkstemp 先分配路径，再接管 fd）。
- **两种等待方式**（§7）：
  - 帧局部：`fs::StatOp op{loop, path};` 写在 `CO2_BEG` 局部列表里，`CO2_AWAIT_SET(r, op);`——
    零分配，可重复等待，两次等待之间经 `op.args()` 修改参数（在途时调用是契约违规）；
  - 右值：`CO2_AWAIT_SET(r, fs::stat(loop, path));`——`uv_fs_t` 超出 co2 内联槽，一次堆分配。
- **File**：只可移动的 fd 所有者。`fs::open(loop, path, flags, mode = 0644)` 返回 `Result<File>`；
  `read(buf, offset = -1)` / `write(buf, offset = -1)`（-1 为当前位置）返回字节数，
  `sync()`、`truncate(size)`、`stat()`、`close()`（File 立即变空，awaiter 持有 fd），
  `release()` 交出 fd，`reset()` 同步关闭。close awaiter 自构造起拥有 fd，不可取消，
  未等待时其析构同步关闭（readFile/writeFile 也使用此协议）。**File 析构时若仍打开则同步关闭**（`uv_fs_close` 无回调，
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
- **迟到的取消**（借鉴 kotatsu 的 Release 钩子）：以完成回调读取 stop_token 为线性化点，
  即使取消投递尚未执行，已请求的停止仍生效；请求成功完成时，
  产生资源的操作在回调里同步释放资源并报告 `Errc::canceled`——`open` 关闭 fd，`mkstemp` 关闭 fd
  并删除文件，`mkdtemp` 删除目录。不产生资源的操作照常报告真实结果（副作用已经发生，例如
  unlink；read 读到的数据也照常返回）。
- **Loop 析构**：fs 请求不是句柄，`~Loop` 的 `UV_RUN_ONCE` 循环会把在途请求驱动到完成，
  等待者照常拿到结果——没有 `Errc::aborted` 路径（被阻塞的请求，例如打开没有写端的 FIFO，
  会让 `~Loop` 一直等下去）。
- 测试在 io_uring 开 / 关两种模式下各跑一遍（ctest 的 `UV_USE_IO_URING=1` / `=0`）。

#### FS watchers（handle 通知层）

- `FsEvent::watch(loop,path,flags)->Result<FsEvent>`；成员 `watch(path,flags)->Result<void>` 重启。
  `next()->Result<FsEventInfo{std::string filename,int events}>`；events 为 UV_RENAME/UV_CHANGE 位。
- `FsPoll::start(loop,path,interval=1s)->Result<FsPoll>`；成员 start(path,interval)、setInterval、interval。
  `next()->Result<FsPollChange{uv_stat_t prev,curr}>`；负状态单独作为 Result 错误，不读取无效快照。
- 共用 detail/watcher 的 EventPort/BoundedFifo + typed Backend，以及 HandleBlock/HandleRef/WaitSlot/CancelHook；fs/detail/watch.hpp 仅保留兼容别名。不共用 FsOp（native handle 而非 req）。
  固定 32 条 FIFO，相邻同名 event 合并位、相同错误合并；poll 成功快照不合并。满时丢最旧，overflow/coalesced
  饱和计数可观测；stop 清空缓存保留计数，成功 restart 换新块并归零。错误/投影异常同样占队列容量，可能被丢弃。
- next 单等待者（含 done 未 take 的预约）；预取消不取缓存，运行中取消不停止监控；第一个 loop 完成赢迟取消。
  stop/close/restart/~Loop → aborted 延后恢复；native 回调内联恢复后不再访问控制块。关闭前已构造的 next lease 保活，
  Loop 已销毁后不准启动新操作；直接析构仍挂起的所有者沿用 Handle 契约违规。
- FsPoll 首次成功 stat 仅建基线；错误不停止监控，路径恢复后继续通知。interval 限 1..UINT_MAX 毫秒，
  多秒间隔更可移植。更改间隔/路径是新代次，会重建基线；不用同 native handle 的立即 stop/start，避免旧 libuv
  在 stat 在途时复活旧 ctx。新块创建失败保留旧配置/缓存；原请求通过旧块的 uv_close 排空。
- ENOSYS init 不调用 uv_close；FsEvent STAT/WATCH_ENTRY 明确 ENOTSUP，recursive 只在 macOS/Windows 放行。
  QNX 无 native fs_event 时工厂返回 ENOSYS，可由调用者显式选择 FsPoll，不自动替换语义。

### 4.9 process（v0.1 之后的扩展）

`uvco/process.hpp` 提供 `spawn(loop, ProcessOptions)` → `Result<Process>`。
Process 只可移动，HandleBlock 控制块持有 uv_process_t。

- 参数：file、args（不含 argv[0]）、env（完整替换）、inheritEnv、cwd、stdio、detached、setUid/uid、setGid/gid。
  空 env 默认继承；inheritEnv=false 且空 env 表示空环境。
- stdio：ProcessStdio::ignore、inherit(fd/Pipe)、createPipe(Pipe&, childReads, childWrites)。
  调用者拥有父端 Pipe；方向按子进程视角。Pipe 必须来自同一 Loop，createPipe 要求尚未打开。
- pid()、kill(signum)、wait() → Result<ExitStatus>。退出回调缓存结果；可重复等待，但同一时间只允许一个
  活跃 wait（包含完成但未恢复的阶段）。回调内联恢复，取消/close 经 Loop 调度恢复。
- wait 取消返回 canceled，不杀子进程；预停止优先于缓存退出结果。取消后可再次等待。
  awaiter 从构造起保活控制块，可以跨 close；开始等待已关闭控制块返回 aborted。
- **析构：后台 reap-only**。不杀子进程，放弃所有者后控制块继续登记在 Loop 上，保持 ref；
  退出后由 libuv 回收，自动关闭 handle。drain 会等到此类子进程退出。
  带 pending wait 析构仍为契约违规，应先取消并恢复或显式 close。
- **close：保留回收责任**。中止 pending wait 为 aborted，close awaiter 等待子进程退出/回收/handle 关闭。
  不想等待可显式 kill 后 close；关闭本身不发送信号。
- **detach()：显式移交回收责任**。返回 pid，立即停止 libuv 监视并关闭 handle；POSIX 调用者需自行 waitpid。
  ProcessOptions.detached 只设置 UV_PROCESS_DETACHED，与这里的所有权移交 API 不同，不自动 unref。
- **Loop 先析构策略**：中止等待为 aborted，但保留 process 监视直到退出并回收，再关闭 handle；
  没有隐式 kill，所以 Loop 析构可能无限等待未退出的非 detach 子进程。这是选择保留回收责任的明确代价。
  之后仍存活的 Process/awaiter 持有孤儿控制块，只允许释放/析构。
- 启动失败没有子进程，直接关闭已初始化的 handle，不等待不存在的 exit callback。
- WaitAwaiter 带 CancelHook，超出 co2 槽；本模块不承诺 wait 零分配。

### 4.10 DNS

`dns.hpp` 提供 `getaddrinfo(loop, string host, string service={}, addrinfo hints={})`
与 `getnameinfo(loop, SocketAddress, int flags=0)`。均基于 ReqAwaiter，支持帧局部左值等待。
host/service 按值拥有；空字符串作为 nullptr，嵌入 NUL 返回 EINVAL；hints 只复制
flags/family/socktype/protocol，不保存调用者的指针链。地址列表按 resolver 顺序复制到
vector<SocketAddress>，不去重、不缓存、不自动连接，仅输出 IPv4/IPv6 地址。

回调入口仅检查一次 stop_token，作为取消/完成线性化点：取消可见则返回 canceled 并丢弃输出，
即使 uv_cancel 因请求已运行而失败；入口之后的取消不覆盖该次完成。addrinfo 由作用域 guard
在 complete/resume 前 uv_freeaddrinfo，包括失败、取消、投影异常。bad_alloc 映射 ENOMEM，
其他投影异常保存在 exception_ptr，await_resume 重抛，不越过 libuv C 回调。
getnameinfo 同样拥有 address 并复制 host/service 输出。测试只使用 NUMERICHOST/NUMERICSERV，
不依赖公网 DNS。系统解析策略、外部 resolver 时延仍由系统/libuv 决定。

### 4.11 Work

`queueWork(loop, fn)` 基于 `ReqAwaiter<..., uv_work_t, R>`。fn 按值持有，可移动不可复制；
优先调用 fn(co2::stop_token)，不接受 token 时调用 fn()。值/void 均支持，返回 Result<R>。
取消：queued 先 uv_cancel，成功则 callable 不运行；busy/已运行则 request_stop 到工作专用
stop_source，worker 自行轮询退出。取消 hook noexcept，不调用额外用户 hook；用户注册在
工作 token 上的 callback 同样必须 noexcept。不强杀线程，after-work 前绝不交付/销毁 request。

worker 中 callable、结果就地构造异常均捕获为 exception_ptr；libuv after-work 发布 worker
写入后回 loop 完成。成功 dequeue 的 UV_ECANCELED 才表示 callable 未运行；callable 一旦完成，晚到取消不撤销结果。异常在
await_resume 重抛。取消成功不回滚已有副作用；忽略 token 的运行中 callable 可以无限阻塞
结构化完成。与 fs、DNS 共用 libuv 全局池，不引入独立 ThreadPool/KeepAlive。

### 4.12 Result-aware all

`whenAllResults(a...)` 复用 whenAny 的归一化、结果收集和 child continuation；RaceState 通过
不可变 stopOnSuccess 策略区分 any/all。all 的成功分支不取消兄弟；首个错误/取消/异常请求
停止兄弟。左至右启动，失败后尚未启动的分支不再启动；全部已启动分支结束后才交付。
异常（包括结果转移异常）> 首个非 canceled 业务错误 > 取消；父取消即使被分支忽略，也在
drain 后报告 canceled。成功 tuple 省略 void，全部 void 得 tuple<>；vector 形式输出
vector<成功值>，void 形式为 Result<void>，空 vector 成功。变参形式至少一个分支。
错误的“首个”以完成状态的原子发布顺序为准；等待者在最后一个分支完成的线程恢复。
co2::whenAll 的 API、Result 作为值的语义、依赖源码均不改变。

### 4.13 P2 可选层

**Process communicate/capture**（process/capture.hpp）：自由函数在原 spawn/Process/Pipe 上组合，
不替换 uv_spawn 后端。CaptureState 拥有三个 Pipe、Process、input 与配置；四个分支并发，
避免写 stdin、等待 exit 或单独读 stdout 时把另一条管道堵满。每个输出有独立 cap，默认各
1 MiB，溢出 ENOBUFS；结果不包含错误时的部分输出。aggregate 的 stop_token 经 CancelHook
回到 loop 关闭 stdio，保证不可取消的 pending write 也走真实 libuv 回调完成。错误/取消/异常
都 drain 所有 IO 分支。join 异常先暂存，让 kill 模式先 await Process::close/reap 再重抛。

stopWaiting 为默认，只停止等待并关闭本次管道，不发信号；Process owner 析构仍交给 ref'ed
后台 reaper，Loop 析构仍可能无限等。killChild/killGroup 显式发送 killSignal（默认 SIGTERM），
成功后等待直接子进程退出；无自动 SIGKILL 升级。killGroup 仅 POSIX 支持并强制 detached
创建新 session；leader 已回收时不再对旧 PGID 发信号，因此不保证处理这时留下的后代。
Windows group 返回 ENOSYS；kill 失败不 abort，返回诊断/保留原业务错误并保留既有回收责任。
后代持有输出 fd 时仍可能延迟 EOF；调用者须使用取消/期限并选择适当的终止策略。

**BufferedReader**（stream/buffered_reader.hpp）：可选、非拷贝、不移动，借用 Stream；核心
stream 不增加缓存。readExactly/readLine/readToEnd 的未消费数据在 cap/EOF/取消错误时保留；
只有成功构造输出字符串后才推进 offset。LF 不交付，CR 保留，最后未换行行会交付一次。
cap 限制输出字节，单次 fill 可额外 read-ahead 4096 字节，已有大缓冲的 capacity 不自动缩小。
一个 reader 只允许一个操作，且不能与底层直接 read 混用。reader/stream 须活过其 Task。

**Descriptor adoption / listener**：Tcp::open/Pipe::open 成功才接管 fd，失败仍由调用者拥有，
不保证 native 调用改变的 flags 回滚。仅包装 libuv 的检查，不额外猜测 fd 类型；
调用者须保证正确类型和唯一所有权，不能依赖 native 拒绝错误类型/非活跃句柄重复接管。
TcpListener/PipeListener 只包装原 Tcp/Pipe，不拆 StreamBlock；连接流与监听角色在新增门面
上分离，旧 API 保持兼容，原并发等待和析构契约不变。

**Directory**（fs/dir.hpp）：Dir 是 shared native-state 的 move-only owner；每个 ReadDirSpec
从构造起持有 lease。内部固定 64 个 uv_dirent_t，每次可请求 1..64 项，复制名字后 req_cleanup。
FsOp 新增可选 noexcept completed 钩子，确保失败、取消、投影异常也释放 busy 预约。
openDir 的投影异常/迟到取消同步 closedir；readDir 只消耗游标，取消失败后的成功仍交付结果。
最后 lease 析构同步关闭；显式 close 同步且检查无在途 read，将 native 指针清空，旧的未启动
操作随后报 EBADF。Dir 可以仅析构于 Loop 之后，但不能在那里启动新操作。

**Next-turn yield**（core/yield.hpp）：独立 ReqAwaiter + LoopResident，内嵌 check/idle 两句柄；
两次 check visit 确保至少跨一轮，idle 驱动避免只有 check 时 poll 阻塞。两个 close callback
完成后才恢复或报告 canceled/aborted；不是精确一次 iteration 或公平性/实时性保证。
原 scheduleOn 与各模块恢复策略不变。

## 5. v0.1 范围（已定）

| 模块 | 包含 |
| --- | --- |
| core | ✅ |
| timer | ✅ |
| sync + whenAny/timeout | ✅ |
| stream（Tcp/Pipe） | ✅ |
| udp | ✅ |
| fs | ✅ |
| dns、work（`uv_queue_work`） | ✅ v0.1 后扩展 |
| process | ✅ v0.1 后扩展 |
| signal、tty、poll | ❌ 后续 |

## 6. 实现顺序（已定）

1. core：`Result<T>`、`Loop`、`run()`、`ReqAwaiter`
2. timer
3. sync：Event / Mutex / Channel / whenAny / timeout
4. stream：Tcp / Pipe / accept
5. udp
6. fs

## 7. 已定的实现决策

- **大请求存储**：`uv_fs_t` 等大请求放在 uvco 自己的 awaiter 中，由协程帧直接持有，
  帧局部左值等待时不经过 co2 的 8 指针内联槽、没有额外 awaiter 堆分配；右值大请求仍可能
  退化为堆存储。不修改 co2 全局配置 `CO2_AWAIT_STORAGE_SIZE`。
- **awaiter 所有权**：HandleRef 从构造起持有控制块，与 opBegin/opEnd 分开；CloseOp 从构造起持有 fd，
  清理关闭不受取消影响。LoopResident 允许 process 延迟关闭至退出（外部 uv_walk 不覆盖其回调）。
- **Loop 所有权**：用户持有 `Loop`；`run(loop, task)` 只负责驱动，同一 loop 可承载多个 Task。
- **依赖引入**：libuv 与 co2 均优先 `find_package`（libuv 还会尝试 pkg-config），找不到时回退到
  FetchContent。co2 的预览版包不接受数值版本号，因此 `find_package(co2 CONFIG)` 不带版本；
  FetchContent 固定到一个提交（`UVCO_CO2_GIT_TAG`，当前为 `0be5ab2f1b72be3755d8d13fb02d4bf32f652539`）。
- **契约而非异常**：`Result::value()` / `error()` 用错状态、在非 loop 线程调用 libuv 相关接口、
  带着挂起请求析构 awaiter、带着存活的 `KeepAlive` 析构 Loop、在 Loop 析构后使用孤儿句柄——
  都是契约违规（走 co2 的契约处理器），不抛异常。（带着打开的句柄析构 Loop 已不再违规，见 §4.1。）

- **单等待者槽**：内部 WaitSlot<T> 统一 idle/pending/done、等待者与 Result 交付；预约从 begin
  保留至 take，完成但尚未恢复也不可重新占用。payload 限 nothrow move；分配型投影不放入此槽。
  HandleRef、CancelHook、opBegin/opEnd 和底层 stop 留在模块内；槽不决定恢复策略。
- **请求异常边界**：DNS guard 在恢复前释放 addrinfo；Work worker 捕获 exception_ptr，
  after-work 后重抛。DNS 保留回调入口取消规则；Work 保留 callable 的完成结果。未发起时仍遵循 ReqAwaiter 的先看取消。

- **P2 存储优化**：whenAny range 用 unique_ptr<Entry[]> 固定存储，每项含 slot/end/exception/child；
  Task vector 独立。只减少 bookkeeping 分配，不改变 eager collection、loser 异常或 drain 语义。
- **libuv 边界**：所有生产头统一经 detail/libuv.hpp 导入，Windows 提前定义 NOMINMAX /
  WIN32_LEAN_AND_MEAN；numeric_limits::max 使用括号避免已有 Windows 宏污染。

- **安装边界**：BUILD_INTERFACE 使用源码依赖，INSTALL_INTERFACE 仅引用包内导入的 dependency bridge。
  导出 uvco::uvco，不导出/安装源码依赖目标；FetchContent 子目录 EXCLUDE_FROM_ALL 防止自动附带安装。
  消费端 co2 总是 find_dependency(CONFIG)；libuv 记住 CMake/pkg-config provider，FetchContent 对应独立 CMake 包。
- **包兼容性**：0.x 使用 SameMinorVersion、ARCH_INDEPENDENT（本库为纯头文件），不承诺跨 minor 兼容。
  source 与 installed config 复用 co2 能力/真实 libuv >=1.40 头文件编译探测；不运行探测程序。
  parent 源 target 传递 usage 显式收集；不能求值的复杂 source generator expression 拒绝而不是误验系统头。

- **FS 通知通道**：FsEvent/FsPoll 为 fallible watch/start 工厂 + HandleBlock/HandleRef/WaitSlot；固定 32 条 FIFO，
  相邻同名事件合并位，相邻同错误合并，满时丢最旧并累计 overflow。计数饱和；stop 清缓存保留计数，
  成功事务式 restart 新控制块并归零。取消只针对 next，完成先于迟到取消；stop/close/restart 延后恢复。
  init 失败直接释放未初始化控制块，绝不调用 uv_close；回调投影的 bad_alloc→ENOMEM，其他异常按条目保存，
  take 完成清理后重抛。原生回调内联恢复后不再访问控制块。

### 统一 watcher 内核与 Signal（Block 2）

- `detail/watcher/delivery.hpp` 是纯缓存策略；`event_port.hpp` 只有一套 WaitSlot/CancelHook；
  `native.hpp` 组合 typed Backend 与 Sink，不新增 public virtual Watcher/union/type switch。
- FsEvent → FsPoll → Timer 分步迁移，原测试不改；fs 旧 detail 名称仅兼容别名。
  `LegacyTimerRules` 显式保留预取消/缓存/active 优先级及 close 不清旧计数，计数回绕为零仍为空。
- 新 Signal 采用单槽饱和归约、独立广播、事务性新代次；SIGPIPE/同步故障/不可捕获及 NPTL 保留信号被拒绝。
  SIGCHLD 只通知；Process/libuv 仍唯一回收其子进程。多 Loop 须位于不同线程，遵守现有 Loop 亲和契约。
- 原生通知仍内联；用户 stop/close/restart 延后恢复；预构造 next 持有旧代次 HandleRef。
  ~Loop 原有 resident 关闭/孤儿策略不变。stream read/UDP recv 不并入通知缓存。

### TTY/console 边界

TtyBlock 复用 StreamBlock（只新增只读 hasReadReservation 查询），public Tty 只提供流数据/模式/尺寸操作，
不继承会导出 listen/accept 的 StreamHandle。duplicate 始终保留 caller fd；native init 失败不 uv_close。
通过公开 uv_fileno 比较 native descriptor 与辅助 duplicate，避免 Unix reopen 泄漏与 fallback 双关。
Windows 先确保 CRT duplicate >2，避免进入 native stdio HANDLE 再复制分支；仍需实机验证。
显式 mode 更改在 read 预约期间返回 busy；beginClose 与 loopClosing 都在关闭前尽力 Normal，
不使用全局 reset；原生 setMode 的阻塞/失败限制仍存在。精确 ModeGuard/独占租约留给后续子步骤。

### Resolver / HE / process-group 生命周期

- ResolverBlock 复用 HandleBlock/HandleRef，producer 由自身 frame/owned key 持有；订阅者 intrusive node 在 awaiter 中，
  从构造保活，取消只在 loop 上 unlink；先完成再排队的取消不覆盖已交付结果。
- 每个 resolver 的 native async 平时 unref，在 flight 存在时 ref；logical stopping 先中止订阅者/取消 producer，
  等最后 flight 的 deferred collector 完成才 close native handle。保持原有 ~Loop 禁止外部 KeepAlive 的契约。
- 缓存成功结果为 immutable shared vector；精确 key/epoch，有界 LRU；缓存分配失败不使成功查询失败，
  bad_alloc 映射 no_memory，其他 backend 异常在 collector drain 后从 await_resume 重抛。
- HE 专用 first-success coordinator 分离 DNS/候选策略/有界 worker；所有 frame 创建好后才启动，
  guarded workers 捕获异常、取消并排空后重抛；父 CancelHook 由 state RAII 兜底 disarm。
- Process privateGroup 只增加 spawn 身份能力，不改变默认 reap；waitid(WNOHANG|WNOWAIT) 检查子身份后 kill(-pid)，
  loop-thread/sole-reaper 保证期间不会由 libuv 回收复用 PID；capture 也走同一检查。

## 8. 待定问题

- uvco core 依赖 co2 的若干 `detail` 接口：`TaskAccess`（启动 Task / 取结果）、`stopTokenOf`
  （从 promise 取 stop_token）、`FrameHeader` / `noopDestroy`（run() 的手写完成帧）。
  已集中在 core/co2_compat.hpp，CMake 对已安装/FetchContent co2 都做能力编译探测；
  仍需跟进其语义变化，未来推动公开“启动 Task”扩展点。
- 已解决：timer/stream/udp/process 的未启动 awaiter 通过 HandleRef 保活，跨 close 不再悬垂。
  Loop 先销毁后仍不得启动需要访问 Loop 的新操作（孤儿契约）。
- ~~定时器回调路径内联恢复、stop()/close() 延后恢复是否要统一~~：已在 §4.5“恢复策略”中统一
  （按唤醒来源区分，timer 现有行为即符合）。
- sync / whenAny 额外依赖 co2 `detail`：`ForwardStop`、`LateInit`、`HandleAccess::header`、
  `TaskAccess::arm/hasError/rethrowError/takeResult`（与 co2::whenAll 的实现方式相同）。
- ~~`timeout` 只接受 `Task<Result<T>>`~~、~~whenAny 只支持同类型分支~~：已由 whenAny 重做解决
  （任意 awaitable、异构分支，见 §4.5）。另依赖 co2 `detail::AwaiterOf`、`ResultStorage`。
- 已解决：whenAny 分支续体收集结果时的 throwing move/construct 不再 terminate。
  tuple/vector 每分支保存 exception_ptr，续体仍 noexcept；将异常按 BranchEnd::exception 记入比赛，
  取消落败者并等待全部已启动分支结束后，从 await_resume 重抛。
- 已提供 Result-aware whenAllResults，复用 whenAny 的 RaceState 策略、归一化/异常收集。
  tuple/range 外壳仍各自维护；与 co2::whenAll 的更深层统一仍应在 co2 侧完成。
- sync 原语不支持跨线程使用；跨线程通信目前只能经 `scheduleOn(loop)` 回到 loop 线程再操作。
- timeout 功能测试已去掉毫秒级耗时断言；独立 timer 精度测试仍保留容差，不代表硬实时保证。
- stream：已有 duplicate-only `Tty`，精确 ModeGuard 仍待实现；Pipe 不支持 IPC（传递句柄）与 Linux 抽象套接字（`uv_pipe_bind2` 需 libuv ≥1.46，
  而最低版本为 1.40）；不能在不关闭流的情况下取消写；connect 取消会关闭流；Windows 行为未测试
  （命名管道、`uv_try_write` 语义不同）。
- stream 依赖 libuv 的若干实现行为（已在 1.48 源码核对）：无 accept 时保留一个已接受 fd 并停止监听、
  `READING` 标志清除后读循环立即停止、关闭时以 `UV_ECANCELED` 回调 connect / write。
- 已解决：Sleep 在请求在途期间登记 LoopResident，Loop 析构走原关闭回调并以 aborted 完成。
- 远程投递（`schedule` / `post`）的线程若既不在 `run()` 期间、也不持有 `KeepAlive`，与 `drain()` /
  `~Loop` 竞争（`~Loop` 发现未处理的远程工作即契约违规）。
- 保活释放竞争测试依赖 GNU ld / lld 的 `--wrap`，只在 Linux 上构建。
- install/export 已完成：GNUInstallDirs、uvco::uvco、可搬迁 Config/Version；依赖不捆绑，消费者重新校验。
  Windows/macOS、多配置生成器与 CMake 3.14 实机验证尚待补充；非 imported 源目标复杂 generator expression
  会明确拒绝，推荐使用依赖的 installed/imported target。

- DNS 已有 opt-in LRU/single-flight 与 HE 核心子集；非权威 TTL、不做负缓存/完整 RFC6724/NAT64。Work 不强制中断，取消不撤销副作用。
- P2 已增加 capture/BufferedReader、目录迭代、UDP multicast 选项、listener 门面与 yieldNextTurn；
  FsEvent/FsPoll/Signal/phase/TTY 已实现；待扩展通用 fd poll 与精确终端 ModeGuard。未更改 Process reap-on-destruct 或可能阻塞 Loop 析构的策略。
- capture 不保留错误时部分输出，无信号升级/跨平台 process-tree 后端；POSIX leader 已回收后
  不再寻址旧 PGID。BufferedReader cap 不是硬 allocator 限额；Dir close 是同步清理。

### Block 2 剩余边界

- Resolver 现在用 resident/native async ref，而非外部 KeepAlive；最后订阅者离开立即取消，无 cache-fill grace。
  close/~Loop 必须等待 producer 最终完成：不可取消的 NSS/注入后端可能阻塞 teardown，不承诺硬 deadline。
- HE 为 RFC8305 核心子集，族内 policy hook 不等于完整 RFC6724；固定最多 64 个 worker，task/slot 为每次连接分配。
  共享 DNS 向 owned 候选数组复制，优先保证独立排序和生命周期，不为少量分配加入复杂 arena。
- POSIX signalGroup 是 best-effort session/group，不是后代 containment；leader 已退出就拒绝旧 PGID。
  缺少 WNOWAIT 返回 ENOTSUP；不支持外部 reaper。Windows Job、完整 RFC6724/NAT64、精确终端 snapshot/租约另行里程碑。
- Signal 外部 disposition 共存无法保证；callback count 不是 POSIX 信号发送数。
- phase ref 不等于 wakeup；Idle 可能空转；无 generator sugar（保留 next 循环），避免未验证的帧取消/销毁语义。
- Windows/macOS 原生 Signal/TTY/phase 尚未本地验证，CI 仍为非阻塞实验 job。

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

以下是初版历史记录；回收/关闭语义已被后文 Review 修复和 §4.9 取代。

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


### Review P0/P1 修复（2026-11-07）

- CloseOp 自构造到启动拥有 fd；忽略预停止和在途取消，未等待析构同步兜底。readFile/writeFile 不再在清理取消时漏 fd。
- HandleRef 独立内部 hold 支持复制/移动，覆盖 tick/read/accept/write/recv/send/connect/shutdown/wait。
  review 的跨 close UAF 和 Sleep 永不完成都转为回归测试。
- fs 回调捕获异常并清理；mkstemp 调整资源转移顺序。Result 构造失败后恢复有效错误态；move-only 值的 copy traits 正确。
- fs 取消线性化为回调读取 token；新增“回调前请求停止，但取消投递未运行”的确定性回归测试。
- Process 改为析构后台回收、close 等待回收、detach 显式移交；Loop 先销毁会等待，可能无限阻塞，见 §4.9。
- 新增可移植 process helper 和基本功能测试；Linux 的 FIFO/procfs/waitpid 测试按平台注册，UNIX 的 stream/req 测试单独守卫。
  helper 作为子进程夹具关闭 sanitizer 插桩，库与父测试仍全量插桩，避免重复加载运行时和信号终止夹具的干扰。
- co2 内部依赖集中 compat，并在配置时能力探测；构造不兼容的本地 co2 fixture 已验证配置拒绝。
- timeout 计时断言改为结果/取消/清理语义断言，运行时间由 CTest timeout 限制。
- whenAny throwing move 已在后续修复（见下）；当时尚未解决 all/any 上游统一、install/export（后者现已完成，见后文）；
  Windows/macOS 仅代码分层，未实机验证。


验证记录：21 个 CTest 项全部通过：g++/clang++ 各重复 5 遍，ASan/UBSan 并行重复 10 遍，
TSan（halt_on_error=1）并行重复 20 遍，单项 timeout=120 秒。包括 io_uring 开/关的 fs 两遍。
三个 build/review 复现分别转为 fs close、延迟 process wait、Sleep teardown 正式回归测试，修复前均观察到失败。
另验证 find_package 的本地安装 co2 能力探测通过，不兼容 fixture 在配置阶段被拒绝。
日志保留在 build/review/verified-*-tests.log；没有 Windows/macOS 执行结果。


### whenAny throwing-move 异常安全修复（2026-11-07）

- 根因：collectOne / range branchDone 虽为 noexcept，但从 Task promise 取值、移入组合器结果槽
  都可能抛异常。只删除 noexcept 会让续体越过剩余分支计数更新，仍可能提前销毁挂起 Task。
- collect 统一在 noexcept 边界捕获所有异常（包括非 std::exception），每分支保存 exception_ptr；
  分支体异常也经过相同路径。原有 RaceState::branchDone 总会执行：记录第一个异常、请求取消、
  递减 remaining。其原子发布/获取链保证 await_resume 读到各分支结果/异常槽。
- 异常优先级保持不变：第一个被记录的异常 > 非取消错误 > 胜者/取消结果。落败者在取消过程中
  发生的结果转移异常也优先于先前成功或 timeout 结果；所有已启动分支完成后才重抛。
- 当胜者从结果槽构造成 WhenAnyResult/WhenAnyRangeResult、再传给调用者时抛异常，已经位于
  await_resume（所有分支已结束），正常向外传播。保留并验证 WhenAnyResult 失败移动后
  源仍有效、目标为空的基本异常保证；不强求恢复用户类型被部分移动的内容。
- 额外异常存储：tuple 为每分支内联 exception_ptr；vector 在启动前分配同长度异常槽。
  这不是零分配优化；构造 awaiter 时的分配失败发生在启动任何分支之前，按普通异常传播。
- 新增 tests/when_any_exception_tests.cpp，19 用例：tuple/vector 胜者与落败者、Task 取值和
  结果槽构造、Result<T> 包装、嵌套 whenAny、timeout 两个方向、最后交付值构造、分支体/返回值
  构造异常、多异常优先级、非标准异常、两线程并发异常及活对象计数。
- 修复前分别执行 7 个主要回归均 terminate；另外 4 个槽构造/包装/并发回归用旧头文件编译也
  terminate。此前已正确处理的分支体/返回值构造和最后交付异常作为保护测试保留。
- 验证：全部 22 个 CTest 项，g++/clang++ 各重复 5 遍，ASan/UBSan 并行重复 10 遍，TSan
  halt_on_error=1 并行重复 20 遍，全通过；所有单项 timeout=120 秒。
  日志在 build/review/whenany-*-tests.log，红测日志 whenany-red-* / whenany-extra-red-*。
- 限制：析构函数须不抛异常；任一已启动分支不响应取消且永不结束时，whenAny/timeout 仍等待它，
  这是结构化并发语义，不通过强行销毁协程解决。all/any 上游统一与跨平台实机验证仍待后续。

### P1 比对建议落地（2026-11-07）

- 按顺序新增 whenAllResults、DNS、Work，再逐一迁移 Timer/stream/UDP/Process 到 WaitSlot。
- whenAllResults 不替换 co2::whenAll，也不引入 kotatsu 的泛型 error/cancel 通道；维持 Result/Errc。
- DNS 从 ReqAwaiter 设计，kotatsu 当前 async 树没有 DNS 模块；getaddrinfo 的 hints 只复制输入标量。
- Work 以传给 fn 的 stop_token 提供协作取消，不额外暴露可能抛异常的 on_cancel 用户 hook。
  worker 异常与结果构造异常都延迟至 after-work 完成后再传播。
- WaitSlot 为 detail，不引入 busy 错误或隐式 close；旧的并发等待/带 pending 析构契约、
  Timer 累积次数、UDP 延后唤醒、Process 缓存退出状态/回收策略全部保留。

验证记录：27 个 CTest 项；GCC/Clang 各 5 遍、ASan/UBSan（开启 leak detection）10 遍、
TSan 20 遍，总计 1080 次 CTest 执行全部通过；所有公共头单独包含构建通过。
DNS Linux fault fixture 使用 wrap new 调用而不是覆盖全局 new/delete，避免与 TSan 的
强符号 allocator interceptor 冲突，不禁用 sanitizer。build-only mutation 检查分别移除
addrinfo guard、破坏 all 的 stop-on-success 策略，两组回归均以断言失败拒绝错误实现。

### P2 比对建议落地（2026-11-07）

- 仅合并 whenAny range 的固定 Entry 存储，明确没有采用 winner-only extraction。
- capture/communicate 采用 free function + 现有 Pipe/Process，独立输出 cap 和显式取消选项；
  不引入 kotatsu 的原生 process watcher/job 后端，不改变默认回收语义。
- BufferedReader 保留为独立借用适配器；fd adoption 失败所有权、readLine EOF/cap 语义明确。
- Dir 复用 FsOp 的 cleanup/release 与新增 completed hook，close 跟随 File 的同步 RAII 风格。
- yieldNextTurn 使用内嵌 check/idle 两阶段保证，不修改 Loop 的 schedule 队列或其他恢复策略。
- UDP 仅增加选项/指标，listener 仅增加类型门面；集中 libuv import 不提高 C++14/libuv 版本下限。

P2 最终验证（Linux）：38 个 CTest；g++ / clang++ 各 5 遍，ASan/UBSan（含 leak detection）
10 遍，TSan 20 遍，共 1520 次全绿；全部公共头独立编译，四构建无警告。
新增目录 fault fixture 验证迟到取消/投影异常关闭资源且不重复释放；capture fixture 在输出
append 边界注入 bad_alloc / 其他异常，断言向调用者重抛前直接子进程 handle 已关闭。
后者只在 Linux + libstdc++ C++11 ABI 注册，关闭该 fixture 的内联以固定注入边界；
不替换 sanitizer allocator、不关闭 sanitizer。普通 capture 测试不受此 ABI 限制。
仅 build/ 副本的 mutation 验证：用 P1 whenAny 实现时固定分配断言失败；移除 capture join
异常暂存/异步回收时“重抛前 process handle 为零”断言失败，当前实现通过。
验证日志为 build/p2-final-*-{build,test}.log；mutation 证据在 build/review/p2-*-proof/。
Windows/macOS 未运行验证；没有新增信号升级、硬内存预算或异步 closedir 承诺。

### install/export 落地（2026-11-07）

保留 CMake 3.14/C++14 下限；PROJECT_IS_TOP_LEVEL 在旧 CMake 上回退到目录比较。
UVCO_BUILD_TESTS / UVCO_ENABLE_INSTALL 顶层默认 ON、子项目默认 OFF，可由调用方显式选择。
GNUInstallDirs 控制 include/lib/cmake；标准 component 名 uvco。包同时提供 Config 与
SameMinorVersion Version；find_dependency 不联网抓源码。FetchContent 依赖只用于构建，
可从其自身 binary dir 单独安装，不由 uvco 的 install 隐式捆绑。

导出目标不含构建/源码绝对路径。Config 在嵌套 find_dependency 前捕获自身 include 前缀，
避免 CMake <3.30 的 PACKAGE_PREFIX_DIR 被依赖覆盖。co2 预览版本无数字请求，消费端重做能力探测；
libuv CMake/parent、pkg-config、FetchContent 路径均验证实际头文件 >=1.40，provider 记录在 Config。
source target 的普通/BUILD_INTERFACE 及传递 target usage 可探测；复杂表达式报出诊断要求 imported 依赖，
而非默默忽略后用另一套系统头通过检查。不改变运行时错误、取消或恢复语义。

新增 package-tests：安装后搬迁前缀，find_package、add_subdirectory、local FetchContent 三种消费方式，
全部编译/链接/运行两个 TU；确认子项目默认不安装、无源码路径泄露、无依赖捆绑。
dependency-tests 用本机真实 libuv 构建 CMake 元数据/本地 source fixture，覆盖 CMake/pkg-config/fetched
provider、co2 不兼容、旧实际头文件（含 metadata 虚报高版本）、0.x 跨 minor 请求拒绝，
以及 parent wrapper 传递 include 与复杂 generator expression 的显式拒绝。

最终验证：Linux / CMake 4.4，40 个 CTest 在 g++、clang++、ASan/UBSan（leak detection）、TSan
四构建各重复 2 遍，共 320 次全部通过，公共头独立编译通过，四构建无警告。
外部消费者传递同一编译器与 sanitizer flags，均实际 configure/build/link/run。
另用 CMake 3.28 配置、安装、构建并运行独立两 TU 消费者，覆盖嵌套依赖改变前缀的兼容路径。
CMake 3.14 的命令兼容性保留但未实机运行；Windows/macOS、多配置生成器同样尚待验证。
红/绿证据：build/install-red.log 为原版无安装产物失败；build/install-transitive-red.log 为
原先 parent wrapper 传递旧头文件被误接受的回归，修复后通过。最终日志 build/install-*-{config,build,test}.log。

### 实现 fs watcher：FsEvent

- 新增 fs/fs_event.hpp、fs/detail/watch.hpp；不使用 FsOp/ReqAwaiter（原生是重复回调 handle，不是单次请求）。
- 构造采用 `FsEvent::watch(loop,path,flags)->Result<FsEvent>`，同名成员 watch(path,flags) 可重启；
  避免 Timer 风格 init 成功契约错误地在 QNX ENOSYS 时 abort。空默认对象仍遵循既有 Handle 契约。
- 原生 init 错误原样返回；STAT/WATCH_ENTRY 明确 ENOTSUP；recursive 只在 Windows/macOS 放行。
- 缓存是有损、有界通知而非日志。32 条、合并和 overflow counter 属于 API 契约；无自动扫描/补偿。
- 公共 restart 事务式替换控制块，失败保留旧 watch；成功中止旧 next 的结果为 aborted。
- 实测：Linux create/modify/rename/delete、跨线程取消、close 未启动 lease、Loop 析构和 fd 稳定性；
  --wrap 覆盖 ENOSYS init / EACCES start 清理、固定容量/顺序/合并、预取消保留缓存、迟取消、投影异常。
  不把 Linux 测试结果解释为 macOS/Windows/QNX 认证。

### 实现 fs watcher：FsPoll

- 新增 fs/fs_poll.hpp；复用 FsEvent 的 bounded WatchBlock，无 worker 结果借用、无私有 libuv ctx 访问。
- `FsPoll::start(loop,path,interval)` 工厂 + 同名成员重启；setInterval 同样事务式新块，不把 native start 的
  “active 时返回成功但不改间隔”误当修改成功，也避免 libuv 1.48 fs-poll.c 的在途 stop/start 旧 ctx 复活。
  代价是每次重启分配控制块/native ctx、重建 stat 基线；旧句柄异步关闭，close/Loop 等待原 stat 清理。
  queue 上限不限制 libuv/OS 自己的缓存或用户不停重启而不驱动 loop 的旧代次；需要正常驱动 loop 排空。
- 负状态交付 Result 错误，watch 保持 active；成功回调复制两份 uv_stat_t 后才能恢复。不同成功快照不合并。
- 测试：真实 missing→create→append→delete→recreate；40 次初始 stat 在途重启、interval 验证、timeout/
  跨线程取消/stop/restart/close/Loop 销毁、构造期 lease、Linux fd 稳定；--wrap 验证栈快照副本、32 条
  FIFO overflow、错误合并、预取消、迟取消、init/start 失败及失败 restart 保留缓存。共享 next 并发契约独立测试。
- 尚未实机验证 macOS/Windows/QNX；fs_event 平台差异、网络 FS 的丢/合并事件不由本封装消除。
  目标自身 rename/delete 不承诺自动跟随/重建 FsEvent；FsPoll 按路径可恢复。stop 不强制中断已提交 stat，
  close/~Loop 可能等待底层 I/O；active watch 保持 loop 存活，drain 前应 stop/close。

### FS watchers 验证边界

- final backend error 即使 native handle 已失活也进入缓存/交付；仅 closing 禁止交付，避免漏终止错误。
- 真实事件测试的 create/modify/rename/delete 使用不同预置文件，避免旧 change 满足下一阶段；
  纯取消/生命周期测试监控静止文件，避开 macOS directory FSEvents 的 start 前事件许可。均无耗时下界断言。
- 故障注入覆盖 wrapper 投影字符串分配、init 整体失败、start 调用边界失败；未注入 libuv 内部每个 malloc 点。
  不依赖 libuv 私有 poll_ctx；native 的部分初始化/allocator 极端失败行为仍由所链接 libuv 负责。

- 本次最终 Linux 树：g++ / clang++ / ASan+UBSan / TSan 四配置，完整构建均包含公共头独立包含检查；
  各 45/45 CTest 完整重复 3 次（每配置 135 次执行），含 install/consumer/两 TU；新增 5 组 watcher
  另各重复 20 次（每配置 100 次执行）。无 sanitizer 报告。日志 build/review/fs-watchers/{gcc,clang,asan,tsan}-*.log。
  此处不继承上轮 CI 接入时的 Release/FetchContent 实测声明；本轮未重跑那些额外配置，未运行远端 CI。

### 统一 watcher 第一阶段验证

新增 delivery 策略/异常记录/typed backend/lease 与 Timer 历史语义测试；未修改既有测试源文件。
迁移完成后 g++、clang++、ASan+UBSan、TSan 各 50 个 CTest 目标全量重复三次通过，含安装/consumer/两 TU/header 检查。
Signal 独立测试进程重复 20 次通过，覆盖同号双 watcher、跨线程双 Loop、SIGCHLD/Process、取消/close/restart 与饱和。
后续模块仍以设计文档为待实现项，不能把通过本地 Linux 测试当成 Windows/macOS 已验证。

### phase hook 与高级 awaitable

Idle/Prepare/Check 共用三个薄 typed backend 与一个 PhaseBlock；只多一个本 watcher 的饱和 sequence。
HookSink 按值持有同步 noexcept callable，默认 unref，不附带 EventPort 的槽/队列/CancelHook。
EventPort 适配采用 latest-only + deferred resume；start 幂等且保留 sequence，stop 清缓存、中止 pending next。
onPrepare/onCheck 自动启动；onIdle 初始停止，避免注册即 CPU spin。高级 facade 默认 ref、初始停止。
Prepare/Check 不提供 poll wakeup/下一轮保证；没有 hidden idle。既有 yieldNextTurn 测试保持通过。
策略统一 offer/take/empty/clear/stats；Latest 覆盖（含相同错误），FIFO 相邻相同错误合并，规则不互相污染。
新增阶段顺序测试用主动启动的 Idle 驱动 Event，不靠毫秒阈值；另测 unref、deferred commit、预取消、stop 与 Loop 先销毁。

### TTY duplicate 实现与保守选择

实现可失败 duplicate 工厂，read/write/tryWrite/shutdown/尺寸/模式沿用 libuv 能力；无网络 listener API。
init/allocation 故障注入证明辅助 duplicate 及时关闭且 caller fd 保留；不 uv_close 未初始化句柄。
Normal 恢复同时覆盖 owner close 与 ~Loop；精确 snapshot guard 未实现，应用须独占 mode 区间。
Hook onIdle 初始停止（需要 start），onPrepare/onCheck 自动启动；这是避免无意忙转的保守具体化。
当前无可派发独立 reviewer 工具；已做本地生命周期/所有权审查，但不能冒称独立代码 review 已完成。

### Block 2 前半段历史验证（8238d63）

在新工作目录中，g++ Debug / clang++ Debug / clang++ ASan+UBSan / clang++ TSan 各通过
**54 个 CTest 目标 × 3 次**；其中 14 个 watcher/迁移/Signal/phase/TTY/故障目标各再重复 **20 次**。
ASan 开启 leak 检测，UBSan/TSan halt_on_error；无 sanitizer 报告。含全部独立头、安装/搬迁消费者与两 TU ODR 检查。
原测试源文件保持不变，仅新增测试并登记 CMake。CI 无需调整，现有 matrix 自动构建新目标；未 push。
这是 watcher/Signal/phase/TTY 的历史 checkpoint；后续 Resolver/HE/group 与自查结果见下。


### Block 2 后半段与全量自查

- 依次交付 bounded opt-in Resolver → HE core subset → POSIX privateGroup/signalGroup，未更改五项批准默认值。
- 保守偏差：native resident ref 代替外部 KeepAlive；最后订阅者立即停止 producer（不加 1s grace）；
  HE policy 仅族内 hook，不虚报系统 RFC6724；Windows Job、严格树 containment、精确 TTY guard 延后。
- 对 461b27d..HEAD 做本地 fresh-eyes 自查（不是独立 reviewer）：发现并用 RED→GREEN 回归修复
  capture stale-PGID 窗口、HE setup 异常留下 CancelHook、HE loser drain 期间 Loop-first 返回孤儿成功、
  Resolver 用户 callback 重入 close/invalidate 后继续启动 producer/重发旧缓存。
- 架构判断：watcher 只统一后端/交付/生命周期；stream/UDP 的背压与单次 req、DNS single-flight、HE 比赛不强塞 EventPort。
  没有新增大而全 Watcher 虚接口或 hook std::function；更多复用应由真实重复驱动。
- 全量测试与平台边界以 docs/block2-review.md 的最终记录为准；本地 Linux 验证不代表 Windows/macOS/QNX 已验证。

最终后半段 + 全量自查源码已验证：g++、clang++、ASan/UBSan、TSan 各 59 CTest targets ×3，
再各 18 个定向 targets ×20，全部通过；包含安装/消费者/headers/两 TU。日志为 build/block2-cont/verified-*.log。


### 独立 review 后的规则修订

- pending write 的 token cancel 默认 close 整个 stream；等 native write callback 后 canceled，
  不提前释放 buffer。pre-cancel 无副作用；已完成 write 不被晚到 stop 覆盖，显式 close 仍 aborted。
- write Result<void> 不提供进度：try_write 发送前缀后 uv_write 提交/分配失败关闭流；
  queued write callback error 也关闭流。用户不得把未知部分提交当作可重试的整包。
- 双族解析全败：EAI_AGAIN 优先，避免永久 miss 掩盖暂时性故障；否则优先非 miss，稳定回退 IPv6。
  已启动连接全败仍按最后发起候选的错误，不改 first-success/drain。
- Work queued cancel 由 uv_cancel 成功跳过 callable；运行中 stop 只作协作通知。
  函数完成即返回值/void，异常 after-work 后重抛，不再按父 token 丢弃真实结果。
- 新增 Tcp/Udp/Timer/DnsResolver::create() Result factory。native 错误原样 Errc、内部 OOM 映射 no_memory；
  legacy handle 构造器保留 native init fail-fast，DnsResolver throwing ctor 保留 native system_error 身份。
  新资源边界推荐工厂，参数自己的 C++ 构造异常不被掩盖。
- Loop 显式 scheduleCapacity（默认65536/queue），四个 vector 构造期 reserve，native init 前完成。
  schedule 与 batch drain 零分配；超过配置上限是契约违规，不掉 continuation、不重入 resume。
  这用有限/可调内存换取 noexcept 路径可证明不抛；未来 co2 有公开 intrusive scheduler node 后可再去掉此上限。
- ExitStatus 字段在 0.1 前统一为 exitStatus/termSignal，无兼容别名。
- 已加入 MIT/CHANGELOG 与 installed license 回归；格式提交仅四个指定 header + .clang-format，
  非注释 C++ token stream 前后相同。验证历史从 README 移到 verification.md，不依赖临时目录。

### KCP 可选层补充（§7/§9）

默认OFF，`uvco::uvco`仍header-only；显式 `uvco::kcp` 链接原样固定版本vendor的C静态对象，
通过uvco-owned adapter将IOFFSETOF替换为标准offsetof，原始文件/许可/哈希不变。
Core无Loop/clock/socket，Session/Endpoint负责native timer、共享Udp泵和有界队列。
详见 [design-kcp.md §15](design-kcp.md#15-后续实现与最终契约)。

HandleBlock的CloseResumePolicy默认为inlineNative（既有模块不变）；KCP显式选择deferred，
保证native close通知也经Loop schedule queue。~Loop仍以resident/native anchor保证内部pump排空，
不持有跨析构的外部KeepAlive。KCP的busy返回与既有流并发契约不同，是新API的显式单waiter规则。
