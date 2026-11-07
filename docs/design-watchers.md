# Watcher 统一架构与 Block 2 模块设计（已批准）

状态：**用户已批准，分阶段实现中**。五项默认已确认：hook 默认 unref、拒绝 SIGPIPE、
DNS cache 默认关闭、HE 核心子集、kill-tree 先 POSIX group 后 Windows Job。
统一内核、FsEvent/FsPoll/Timer 迁移、Signal、phase、TTY、DNS cache、HE 核心子集与 POSIX signalGroup 现已交付。下文为批准设计；实际 API/保守偏差以 README、architecture §7/§8/§9 为准。
基线：`main@48d201f`；工作目录 `/home/lvyues/code/uvcoro/co_uv`。
核对：2026-11-07（北京时间）。所有 API 片段均为拟议接口。

## 1. 结论：统一机制，不抹平语义

推荐 **typed backend + delivery policy + sink**，不是“所有东西继承一个大 Watcher”。

```text
HandleBlock / HandleRef                 原有 native 生命周期、构造期 lease
  └─ NativeWatcherBlock<Backend, Sink, LifecycleRules>
       ├─ Backend                      init/start/stop/active/native，投影一次回调
       └─ Sink
            ├─ EventPort<E, BufferPolicy, ResumePolicy>
            │    ├─ WaitSlot<E> + CancelHook
            │    └─ 有界缓存 / 计数 / 合并 + 延迟投影异常
            └─ HookSink<F>             同步 noexcept 回调，不假装有异步等待者
```

- **Backend 不管理等待者；缓存策略不调用 libuv、不恢复协程；恢复策略不决定数据如何丢弃。**
- EventPort 复用 WaitSlot 状态机，不复制 idle/pending/done；opBegin/opEnd、close 和 holds 仍由控制块负责。
- typed native handle，不用 kotatsu 的 runtime union/type switch；不新增虚拟 public Watcher 基类。
  除现有 HandleBlock 的虚函数外无需新增 type-erasure。公共默认 facade 简短稳定，模板留在 detail。
- 首版只稳定默认 facade；策略在内部可编译期替换、独立测试。是否公开 BasicWatcher/第三方策略接口待决定。

另两种方案：①每模块复制 awaiter/queue，短期便宜但继续失配；②统一 void watcher + runtime kind/queue，
实现集中但丢失 payload/策略类型信息、每类付最大存储成本。均不推荐。

### 边界

Timer/FsEvent/FsPoll/Signal/Idle/Prepare/Check 都是重复通知，适合此核心。
**Stream read/UDP recv 不并入**：它们交付调用者缓冲区、需要内核背压；UDP 还需延后停止/唤醒。
不能把它们改成“无人接收时也读入 mailbox”，否则破坏零缓冲/零拷贝与丢包契约。
继续只共享 HandleBlock/HandleRef/WaitSlot/CancelHook；将来真有重复再提取更小的完成辅助函数。
Process 的 sticky exit/reap、DNS/work 的单次请求也不伪装成 watcher。

拟议文件边界：`detail/watcher/{delivery,event_port,native}.hpp` 仅依赖 core；
`watcher/{signal,phase}.hpp` 与新 `watcher.hpp` 导出新 facade，不反向包含 timer/fs，避免循环。
既有 timer/fs 复用内部内核；`stream/tty.hpp` 依赖 stream；`dns/{resolver,cache}.hpp` 不依赖 TCP；
高层 `net/connect.hpp` 才组合 dns+stream+timer；`process/{group,managed}.hpp` 保持独立。
新 co2 兼容访问只经过 co2_compat，不在各模块复制 detail 调用。

## 2. Delivery policy：编译期、独立、有界

统一缓存记录 `DeliveryRecord<E>{Result<E>, exception_ptr}`。Backend 先投影，再 offer；
有等待者则从策略取一份并 complete，否则保留。策略协议：`offer/take/empty/clear/stats`；
offer/take/move 必须 noexcept，E 的 Result 必须 nothrow move，与现有 WaitSlot 约束一致。
Timer 每次投影 `uint64_t{1}`，Signal 每次投影 `{signum,1}`，无需 input/output 两套类型。

| 类型 | 默认策略 | 交付/容量 |
|---|---|---|
| Timer | ReduceOne<Add, LegacyWrap> | 累计后一次取走 uint64_t；保留现有溢出算术 |
| Signal | ReduceOne<SignalCount, Saturate> | `{signum,count}`，饱和及诊断；不是每个信号分配节点 |
| Idle/Prepare/Check 的 awaitable facade | Latest<PhaseEvent> | 1 条，覆盖旧值；sequence 是该 watcher 回调序号，不冒充全局 turn |
| FsEvent | BoundedFifo<E,32,AdjacentNameMaskMerge> | 相邻同名事件合并位；满丢最旧 |
| FsPoll | BoundedFifo<E,32,NoValueMerge> | 成功 stat 转换保序；满丢最旧 |

- FIFO 的相邻相同 Errc 合并；带 exception_ptr 的记录不合并，保持现有 fs 行为。
  Latest 覆盖旧记录；ReduceOne 仅归约兼容成功值，不兼容记录按单槽覆盖并记 dropped。
  默认 Timer/Signal/phase 的原生回调没有错误 payload，这一分支不会吞掉它们的正常计数。
- 共用 stats：buffered/dropped/overwritten/coalesced/saturated；诊断计数饱和。
  Fs 的 queued/overflowCount/coalescedCount 仍是兼容转发，原数值含义不变。
- 缓存错误/投影异常同样占容量且可能被丢弃；取消/aborted 是等待操作的控制结果，不塞入事件缓存。
- bad_alloc 投影为 ENOMEM，其他投影异常保存到记录；next 先 disarm/take/opEnd，再重抛，绝不穿越 C 回调。
- Latest/Reduce/固定 FIFO 自身不分配；一个控制块分配仍存在，FsEvent 文件名、异常对象、用户 Task 帧可能分配。
  不承诺整个 libuv/OS 后端零分配，也不承诺字节级内存上限。自定义大容量策略不应默认实例化到所有 watcher。

## 3. 公共 API 与不变量

```cpp
// 新模块采用可失败 create；start/stop 返回 Result<void>，close 沿用原有 CloseAwaiter。
auto made = uvco::Signal::create(loop);          // Result<Signal>
// 检查 made；以下在协程中，r 为 Result<SignalEvent>：
if (!made->start(SIGINT)) { /* 处理错误 */ }
for (;;) {
    CO2_AWAIT_SET(r, made->next());              // 从当前 Task 继承 stop_token
    if (!r) break;
    // r->signum / r->count：累计的 libuv 通知，不等于内核实际发送次数
}
CO2_AWAIT(made->close());

// 兼容旧 API，新增同义操作，不删除旧名称/签名：
CO2_AWAIT_SET(ticks, timer.next());             // tick() 的精确别名
auto started = fsEvent.start(path, flags);     // 检查结果；成员 watch 的别名
```

1. 单等待者；预约保留至 await_resume，完成但尚未 take 也不可占用第二次。并发 next 继续是契约违规，不改 EBUSY。
2. HandleRef 在 next 构造时持有，移动 facade 不影响它；旧代次 lease 不暗中迁往新 watch。
3. live watcher 上预取消不消费缓存；运行中取消只撤销这次等待，watch 继续。
   native 完成与 CancelHook 在 loop 上先完成者赢，迟到取消不改已交付结果。
4. stop 清缓存、保留诊断；close/~Loop 中止等待为 aborted，经 loop **延后**恢复。
   native 完成使用 ResumePolicy；Timer/fs 仍内联，恢复后不再访问 self。
   notify 只拒绝 closing，**不能用 uv_is_active 过滤末次通知**（一次性 timer/已失活后端的末次错误）。
5. native init 失败只释放未初始化对象；成功 init 后 start 失败按原关闭流程收尾；绝不 uv_close 未初始化 handle。
6. ~Loop 先停止/关闭所有登记对象并驱动 close，等待操作被中止；持有的对象成为 orphan，只准 close/析构。
   不能从 hook 中嵌套 run 或析构 Loop。异步 native I/O/子进程可能使兜底 teardown 等待，不承诺强行中断。
7. 仍遵守“有挂起 next 直接析构 owner 为契约违规”；显式 close 可以交接控制块并中止等待。
   不照搬 kotatsu 的“owner 析构自动 abort pending”，那是另一次行为变更。

**统一的是操作名与核心不变量，不强行破坏历史签名**：Timer::stop 仍 void，Fs::stop 仍 Result<void>；
泛型工具可用 `stopWatcher(w)->Result<void>` 适配。close 本来不报告 native 错误，保持 void await_resume。
现有 Timer/Fs 不同的 restart、关闭后错误优先级由薄 LifecycleRules 兼容层保留，不塞进 BufferPolicy。

异步迭代首版就是上面的 next 循环。以后可加 opt-in `events(w)`，但须验证 co2 generator 在 break/取消时
不会销毁仍挂起的帧；必须捕获 HandleRef 而非 facade 裸引用、明确绑定旧代次，并定义错误后终止/继续策略。
C++14 没有原生 for-co_await；为语法糖新引入常驻 Task/每事件分配不划算，本轮不把 generator 作为基础层。

## 4. Signal 与 loop phase：不能机械模仿 timer

### Signal

- 用 uv_signal_t，**不另装 sigaction handler、不在 OS signal handler 中恢复协程/分配**。
  libuv 在 loop 上投影 SignalEvent；每个 watcher 各有独立计数。多个 watcher/loop 监听同 signum 是广播，不是竞争消费。
- 标准信号与 libuv 均可能合并；count 只能表示收到的 callback 数，不能用于精确作业/退出计数。
- 成功重新 start 建新代次、清计数并延后 abort 旧 next，失败保留原监听；同 signum 再 start 也按 restart 定义。
- POSIX 拒绝不可捕获的 KILL/STOP，拒绝 BUS/FPE/ILL/SEGV 这类同步故障信号及 Linux NPTL 保留号。
  SIGCHLD 可作**只读通知**，不调用 waitpid；Process 仍唯一负责其子进程回收。
  Signal(CHLD) 与 Process.wait 的先后顺序不保证，绝不能看到 CHLD 就 waitpid(-1)。
- signal disposition 是进程级资源：不能承诺与用户自装 handler、其他 signal 库无损共存/自动恢复任意旧 handler。
  **建议首版禁止监听 SIGPIPE**，避免破坏 Loop 已建立的 SIGPIPE-ignore；要支持须单独设计进程级 disposition 协议，
  不能简单 save/restore（会覆盖别的库后续修改）。
- Windows 默认仅接受可实际接收的 INT/BREAK/HUP/WINCH；对 libuv“注册成功但收不到”的 TERM/KILL 等明确 ENOTSUP。
  raise()/abort() 不等价于 console event；HUP 清理窗口约 10 秒；raw TTY 下 Ctrl-C 通常是输入而不是 SIGINT。

### Idle/Prepare/Check：hook 优先、awaitable 为高级观察适配

推荐保留两种薄 facade，共用 Backend/HandleBlock，不让同一实例同时有 hook 与 next 两种消费者：

```cpp
auto hook = uvco::onPrepare(loop, [](uvco::PhaseEvent e) noexcept {
    // 短小的同步观测/预算更新；F 按值存入控制块，不用 std::function
});                                             // Result<PhaseHook<...>>，RAII stop/close

auto phase = uvco::Check::create(loop);          // 高级 awaitable 适配
if (!phase->start()) { /* 处理错误 */ }
CO2_AWAIT_SET(seen, phase->next());              // Latest<PhaseEvent>；不是“保证下一轮”的 yield
```

- Hook 精确在 native phase 执行；F 必须 noexcept，按值持有，除控制块外不强加分配。
  hook 不使用 WaitSlot，因为没有等待者；它共享生命周期，而非为了统一而制造虚假协程。
- Awaitable 的 Idle/Prepare/Check 用 EventPort，默认延后恢复，交付“观察到的 phase”，不承诺 await_resume 仍处于该 phase。
  在 phase 内执行精确逻辑用 hook，不把 arbitrary coroutine continuation 放入 poll 前/后热路径。
- **Prepare/Check 不会自己保证下一轮有进展**：没有 I/O/定时器时 poll 可能阻塞；保持 ref 也不等于唤醒 poll。
  不暗中加常驻 idle 强迫旋转。真正跨轮让出继续用现有 yieldNextTurn，其调度语义不变。
- Idle 每轮都会运行并令 poll timeout=0，不是“有空才调用”；必须显式启动，文档警告 CPU/busy-spin，停止无人使用的实例。
- 建议 hook 默认 unref（纯观察不延长程序生命），awaitable watcher 默认 ref；公共 options 明确 ref 模式。
  unref 不会屏蔽活跃 idle 在其他任务驱动期间的旋转行为。stop_token 针对 awaitable next；hook 用 RAII/stop，不假装有异步取消。

## 5. 现有模块迁移：先冻结契约，逐个替换

当前依据：timer/timer.hpp:62–103,162–191；fs/detail/watch.hpp:26–176；
fs/fs_event.hpp:90–123；fs/fs_poll.hpp:67–119（路径均相对 include/uvco）。

| 模块 | 迁移后必须保持 |
|---|---|
| Timer | tick 仍累计 uint64_t，一次取清；start 清累计但**不 abort 已挂起 tick**；stop 清累计并延后 aborted；stop 返回 void；构造方式、预取消优先级、一次性到期后行为、现有溢出算术不变 |
| FsEvent | FIFO32、相邻名称 mask/相同错误合并、满丢最旧、诊断定义、投影异常、ENOSYS/flags 行为不变；restart 新块、成功 abort 旧 next、失败保留原 watch |
| FsPoll | 两份 stat 副本、成功不合并、错误可恢复、interval 验证、restart 新基线/旧 native 请求 drain 不变；绝不退回同 uv_fs_poll_t 立即 stop/start |

顺序：先把当前 fs queue/wait 机械抽出为 core/detail 策略内核 → FsEvent → FsPoll → Timer（ReduceOne + legacy rules）→
再加 next/start 别名。每一步独立提交、可回退，不顺手“修正”溢出/析构/错误优先级等历史行为。
现有 Timer 关闭后 lease 的具体结果与 fs 不完全相同；先补 characterization tests，保留差异，不趁抽象升级换错误码。
具体兼容点：当前 Timer tick 先检查取消，再看累计值，再检查 active；没有 fs 那样的 closing 优先判断。
因此关闭后尚未启动的 lease 可能消费旧累计值，否则返回 invalid_argument；fs 则优先 aborted。
LegacyTimerRules 必须保留该次序，不能让通用 EventPort 的 closing 判断提前截断。
LegacyWrap 的“空”也须保持 count==0（回绕至0不交付一次成功0），不只保留加法运算。
Timer 计数改饱和、统一 restart/关闭结果都只能另提破坏性变更。

## 6. TTY/console：stream 的扩展，不是键盘事件 watcher

- `TtyBlock : StreamBlock`，复用 read/write/tryWrite/shutdown/close、构造期 lease、stop_token 与零缓冲规则。
  facade 不提供 bind/listen/accept/connect；按 input/output 能力返回 ENOTSUP。shutdown 的 native 平台语义不强造 socket half-close。
- 首版拟议 `Tty::duplicate(loop, fd)->Result<Tty>`：不拿走调用者 0/1/2 或其他 fd；失败也不关闭原 fd。
  **不是直接把 fd 塞入 uv_tty_init**：Unix 可能 reopen 并 dup2，Windows 对 0..2 与 >2 的所有权处理不同。
  平台适配必须跟踪临时 duplicate 与最终 native fd 的身份（公开 uv_fileno），防止 reopen 后遗留 duplicate 或双关。
  先只支持 duplicate；adopt 另行审查，不能类比 Tcp::open 的所有权。dup 不能保证 terminal mode / fallback O_NONBLOCK 完全隔离。
- `getSize()->Result<TerminalSize>`；`setMode(Normal/Raw/Io)->Result<void>`，最低 libuv 1.40 不暴露更新版本才有的 mode。
  可选 `ModeGuard` 明确保存/恢复范围；mode 是终端/console 共享状态，多个 owner 改 mode 应拒绝冲突/要求显式独占。
  setMode 在挂起 read 时首版返回 busy，避免重启读和用户缓冲生命周期混乱。
- 关闭路径在 native close 前做 best-effort Normal；另提供显式 restore()->Result<void> 以观察错误。
  不在每个 Tty 析构时调用进程级 uv_tty_reset_mode，也不承诺 libuv Normal 等于精确恢复所有外部改动；
  精确 snapshot guard 需 POSIX termios / Windows console mode 平台层及独占租约，是后续子步骤。
  TtyBlock 需覆盖 beginClose 与 loopClosing 两条入口，在 uv_close 前恢复；不能只在 facade 析构里做，漏掉 Loop 兜底。
- Resize 用独立 Signal(WINCH)+getSize 组合，可选 Latest<Size> 适配；不缓存无限键盘事件，不自行造终端模拟器。
  重定向 stdio 用 uv_guess_handle 明确交给 Pipe/File，不把 pipe 误当 TTY。

## 7. DNS cache + connect(host,port)：独立 resolver 与专用连接协调器

```cpp
uvco::DnsResolver resolver{loop, cacheOptions}; // loop-affine、容量有界、可显式关闭
CO2_AWAIT_SET(addrs, resolver.resolve(host, service, hints));
CO2_AWAIT_SET(tcp, uvco::connect(loop, resolver, "example.com", 443, connectOptions));
// Result<Tcp>：新 socket；现有 Tcp::connect(SocketAddress) 保持不变
```

### DNS cache

- 不改变原有 getaddrinfo/getnameinfo；新 resolver 以 ReqAwaiter 为 producer。
  key 包括 host 原字节/末尾点、service、family、socktype、protocol、flags、network epoch；不做有损 IDNA/zone 归一化。
- per-resolver LRU，建议显式启用时 maxEntries=256、maxCacheBytes=4MiB、positive maxAge=30s；默认不负缓存。
  **getaddrinfo 没有 authoritative TTL**：这是本地 maxAge，不能叫 DNS TTL；默认 cache 关闭由用户决定。
  oversized 结果可不入缓存，仍正常交付；单调时钟过期，invalidate()/network epoch 清理，不共享跨网络 RTT 历史。
- 命中交付 immutable owned AddressList（共享结果，避免每个订阅者复制 vector）；缓存命中本身不重新分配地址列表，
  但不承诺 Task 帧零分配。同 key single-flight；producer/in-flight/subscriber 数量设上限，溢出返回 busy，不排无限队列。
- 取消一个订阅者不取消别人。最后订阅者撤销后 producer 独立持有 host/service/结果状态，短暂保留回填机会（建议 1s），
  再请求 uv_cancel；运行中的 getaddrinfo 不能强停。实际 producer 用 HandleBlock resident/native async ref 追踪（不用外部 KeepAlive），绝不借用已经返回的调用帧。
  resolver close/~Loop 取消订阅并 drain producer，可能等待底层 NSS。原始 DNS ReqAwaiter 的取消线性化不改变。

### Happy Eyeballs（RFC 8305 的核心算法；不虚报完整符合）

1. 数字地址绕过 DNS。AAAA/A 对应的 AF_INET6/AF_INET 请求相继立即发起；不等两边齐再连接。
   IPv6 先有结果即试；IPv4 先到最多等 resolutionDelay=50ms，AAAA 失败/空则提前继续。
2. 去重后按地址策略排序，族间交错、first-family-count=1；迟到结果插入尚未尝试部分，不取消已开始的候选。
   保留各族系统排序；跨族排序提供 AddressPolicy 插口。**仅交错两个 gai 列表并不满足 RFC6724 的完整 MUST 排序**。
   跨平台系统 policy/source viability 获取应另做后端；首版明确为 RFC8305 核心子集，不宣称完整规范实现。
3. 每次一个新尝试，默认 attemptDelay=250ms；最小间隔默认100ms，用户配置绝不小于 RFC 的10ms，建议上限2s。
   快速失败可提前补位，但仍守最小发起间隔；每个候选独立 Tcp，不能多个 connect 争同一个 native handle。
   建议 maxConcurrent=4、perAttemptTimeout=5s、overallDeadline=30s，防止限并发后被黑洞永久占满；这些是待确认默认值。
4. 第一个 TCP 成功为候选 winner，提交前检查父取消/总 deadline；成功提交后不被迟取消撤销。
   停止新尝试，关闭并 drain 其他 connect；所有已发起 connect 的回调/缓冲/Task 安全退出后才返回 socket。
   未结束 DNS 只是解除订阅，由独立 producer 继续 drain，不让不可取消 NSS 阻塞已成功的连接交付。
5. **不能直接 whenAny(connectA,connectB)**：本库 whenAny 首个业务错误会终止比赛，且 loser 错误可能覆盖成功；
   HE 需要“普通失败继续，首次成功赢”。用专用 ConnectRaceState，失败只记入候选表，异常才取消全部并在 drain 后重抛。
   普通 loser 错误不覆盖 winner；清理过程的异常仍须收集，必要时关闭候选 winner 后重抛。
   全败按候选发起顺序选择最后候选错误（不是 callback 时序）；双族纯 DNS 失败优先保留 transient EAI_AGAIN，再非永久 miss，最后按偏好族选错误，附可选 owned diagnostics。
6. v1 不自行实现 NAT64 prefix discovery/local synthesis、DNS push、完整跨平台 RFC6724 policy，避免假造地址；
   原生 DNS64 返回的 IPv6 地址正常使用。若要求完整 RFC8305，以上是显式后续工作，不是默默忽略规范。

## 8. kill-tree：先说清“树”的保证

- 不扫描 /proc、ps、taskkill 输出来假装可靠树，也不新增 waitpid(-1) reaper。
- POSIX 第一层是显式创建的**私有进程组/session**，可用 uv_spawn 的 DETACHED/setsid 建组，但不等于放弃 Process 所有权。
  `signalGroup(signum)` 仅针对自己建立的 group；普通 spawn 不能随便 kill(-pid)。
  它是 best-effort group，不是严格 tree：后代可以 setsid/setpgid 逃逸，组信号成功也不代表全部退出。
- leader 已回收后拒绝仅凭旧 PGID 再发信号；kill(pid,0) 不是可靠身份凭证，不能声称消除 TOCTOU/PID复用。
  libuv 回收与 exit callback 之间也要审查，不对现有 capture 的 exited guard 赋予更强保证。
  grace→force 若 leader 先退出，不能为了杀余留后代而盲目重用 PGID；缺少稳定 containment 时报告能力不足。
- Windows 可靠树域需要 Job Object。**uv_spawn 返回后 AssignProcessToJobObject 有竞态**，孩子可能已生孙进程；
  libuv 自带 global job 还允许 breakaway，不是我们的私有树域，不能借来兑现此 API。
  严格版本需 CreateProcessW(CREATE_SUSPENDED)→Assign 私有 Job→Resume，并处理 nested jobs/权限/失败回滚。
  libuv 无公开接口收养外部创建的 uv_process_t，不能手填私有字段；因此推荐独立 ManagedProcess/TreeControl 后端，
  Win32 wait callback 只 post 到 Loop，再交付 ExitStatus；其内存、wait unregister、Job/HANDLE 与 LoopResident 生命周期单独审计。
- 拟议接口分层：现有 Process::kill 与 reap-on-destruct **不变**；POSIX 提供明确命名的 signalGroup；
  后续 `spawnManaged(...)->Result<ManagedProcess>` 支持 `terminateTree(Force)`，只有 contained backend 才承诺域内树控制。
  不支持严格控制的后端返回 ENOTSUP，不把 Windows UNIX signum 伪装成通用软关闭。
  不默认启用 KILL_ON_JOB_CLOSE；不得让析构突然从 reap 变 kill，kill-on-owner-exit 必须显式 opt-in。

## 9. 实施顺序、验收与待决策

| 顺序 | 交付 | 大致成本/主要门槛 |
|---|---|---|
| 1 | EventPort/Policy 测试 + FsEvent→FsPoll→Timer 迁移 | M；characterization + 行为等价，不新增模块时先过四构建 |
| 2 | Signal，再加 phase Backend/Hook/awaitable 适配 | M；子进程隔离信号、多个同号 watcher、CHLD 与 Process.wait、不靠危险信号测主进程 |
| 3 | TTY duplicate/stream facade，再做 mode guard/resize | M–L；Unix PTY、Windows console/重定向、fd/模式恢复；非 TTY CI 明确 skip |
| 4 | Resolver 注入 + bounded cache/single-flight | M；fake clock/NSS，取消订阅与 producer drain，不依赖公网 |
| 5 | HE coordinator | L；fake resolver/connector/clock 驱动次序、黑洞、单族失败、迟到 AAAA、取消、同时成功 loser close；loopback 集成 |
| 6 | POSIX group 明确能力，再评审 Windows ManagedProcess/Job | L；受控 helper 孙进程、逃逸/leader先退/权限失败；Windows 必须实机验收，不能只加非阻塞 CI 就称支持 |

每个逻辑步骤独立提交；现有四配置 g++/clang++/ASan+UBSan/TSan、重复运行、安装/consumer/两TU/header 检查都须保持。
phase 测试以受控 I/O 驱动，只检查 idle→prepare→poll→check 的可保证关系，不断言同类 watcher 次序或 timer 跨版本位置；
无毫秒下界断言。策略用 POD/投影故障注入测容量、饱和、错误顺序、预取消和 deferred reservation；旧 fs 红例全部保留。

**已获批准的边界（保留原提问作决策记录）：**
1. 接受内部统一、公共 facade 稳定；是否首版就要公开用户可定制的策略模板？推荐暂不公开。
2. 接受 phase hook 优先 + 显式高级 next 适配、hook 默认 unref、awaitable 默认 ref？Idle 不暗中启动，不替代 yieldNextTurn。
3. Signal 首版拒绝 SIGPIPE/同步故障信号；CHLD 只观察不回收；新 Signal restart 清空/abort 旧等待，是否接受？
4. cache 是否显式启用、30s 本地 maxAge/默认无负缓存；HE 50/250/100ms、4并发、5s单次/30s总 deadline 是否合适？
   接受首版标注 RFC8305 核心子集，还是要先完成 RFC6724/NAT64 平台层再发布？
5. TTY 先 duplicate、精确 mode snapshot 独占 guard 后做；kill-tree 先 POSIX best-effort group，严格 Windows Job 单独里程碑，是否接受？

## 10. 与 kotatsu 的取舍及来源

本次核对 kotatsu main `be21bc6cc1988558be5bd2bc1af320d9f37cdb0c`，不依赖上轮印象：

- [watcher.h:13–54](https://github.com/clice-io/kotatsu/blob/be21bc6cc1988558be5bd2bc1af320d9f37cdb0c/include/kotatsu/async/io/watcher.h#L13-L54)
  抽取 timer/signal/phase 公共等待生命周期，值得学；其单 wait 返回 void，第二 waiter 是业务 busy，析构自动 abort，与本项目契约不同，不照搬。
- [watcher.cpp:12–51,64–124](https://github.com/clice-io/kotatsu/blob/be21bc6cc1988558be5bd2bc1af320d9f37cdb0c/src/async/io/watcher.cpp#L12-L124)
  union + missed + type switch；signal 累计、其余 latest，stop 保留 missed。我们采用类型化 policy，保留 Timer 累计取清、fs payload/FIFO、stop 清缓存。
  代价是少量模板实例，收益是不用每次新增模块就扩 runtime switch 或复制队列。
- [process.h:109–124](https://github.com/clice-io/kotatsu/blob/be21bc6cc1988558be5bd2bc1af320d9f37cdb0c/include/kotatsu/async/io/process.h#L109-L124)
  kill_tree 在 Unix 语义是 leader 的 group；它依赖自己的 sys::Child/backend。借鉴能力分层，不假定 uv_spawn 自动给我们同样的身份/Windows job 保证。
- 规范：[libuv Signal](https://docs.libuv.org/en/stable/signal.html)、[Idle](https://docs.libuv.org/en/stable/idle.html)、
  [Prepare](https://docs.libuv.org/en/stable/prepare.html)、[Check](https://docs.libuv.org/en/stable/check.html)、
  [TTY](https://docs.libuv.org/en/stable/tty.html)、[RFC8305 §3–8](https://www.rfc-editor.org/rfc/rfc8305.html)、
  [Microsoft Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。
- 对照最低依赖附近的实现：[libuv 1.48 Unix TTY](https://github.com/libuv/libuv/blob/v1.48.0/src/unix/tty.c)、
  [Windows TTY](https://github.com/libuv/libuv/blob/v1.48.0/src/win/tty.c)、
  [Windows spawn/global job](https://github.com/libuv/libuv/blob/v1.48.0/src/win/process.c)。
  stable TTY 文档包含更新版本 mode，不据此提高本库 1.40 下限或承诺未验证功能。


## 10. 实际交付说明

Resolver 实际为 loop-bound owner，close awaitable 先 aborted 订阅、最后 flight 完成才 close native。
最后订阅者离开立即 stop producer，无延迟 grace；保留 ~Loop 外部 KeepAlive 契约。
缓存默认关闭；成功 immutable shared vector、本地 maxAge、有界 LRU 与单飞，不缓存失败。
HE 采用独立 coordinator + guarded bounded workers，族内排序 hook + 族间 interleave；不是 whenAny 的误用。
POSIX privateGroup/signalGroup 依赖 WNOWAIT 身份检查和唯一 reaper，不承诺 leader 退出后仍能安全升级信号。
Windows Job/精确 TTY snapshot guard/完整 RFC6724/NAT64 仍未交付。
自查和最终验证见 [block2-review.md](block2-review.md)。
