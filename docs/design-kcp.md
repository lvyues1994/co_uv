# KCP 集成设计（已批准；按阶段实现）

## 0. 已批准原则：先固定边界，再讨论 API

1. **纯协议核**：原样 vendor `ikcp.c` / `ikcp.h`，固定
   `b1a7a2101dcbb96017681a500d6b82bbe5a88766`，保留上游 MIT notice/LICENSE。
   `KcpCore` 仅 RAII 管理 `ikcpcb`，无 syscall、socket、clock、Loop、Task 或定时器；时间由参数注入。
   用模拟时钟和有损网络做确定性测试，而不是靠 sleep 测协议。
2. **I/O 只在外层**：`KcpSession` / UDP endpoint 读取单调时钟、做 UDP I/O、设置定时器。
   output 回调只向**有界队列复制**借用的数据报并记账，绝不 syscall、挂起、唤醒协程或重入协议。
3. **不牺牲低延迟**：用 `ikcp_check` 精确安排下一次驱动，不用固定周期轮询替代；支持 input 后主动 flush
   降低 ACK 等待。业务恢复统一进入 Loop schedule queue，避免协议栈重入。
   默认预设为 **nodelay=1 / interval=20ms / resend=2 / nc=0**；turbo 显式选择 **1/10/2/1**，
   不暗中关闭拥塞控制。验收看 RTT 分位数和最大延迟；30–40% / 3x 只是待测假设，不是承诺。

本设计已批准；实现、构建与测试按§13分阶段提交。§12记录已确认决策，不代表所有模块已实现。
依据《KCP 协议源码学习笔记：从 ARQ 到 uvco 集成》（2026-11-08、上述固定版本）和当前工程
`f933b61` 的 include、architecture、design-watchers；上游固定链接见文末。
该提交是 KCP V2（含可插拔 CC）；首版不开放 CC 插件，不能混用 v1 分支的头/源码或旧教程返回值。

## 1. 对齐当前 uvco，而不是照抄研究笔记的概念名

- 当前实际公共类型是 **`uvco::Udp`**，没有 `UdpSocket`；笔记里的 UdpSocket 在本文指现有 Udp，不新增别名。
  用 `Udp::create(loop)`、`bind(SocketAddress)`、`send(peer, ConstBuffer)`、`recv(MutableBuffer)`；
  recv 返回 `Result<Datagram>`（size/sender/truncated），只有一个 recv waiter，在途 send 不能单独取消。
- 协程类型是 `co2::Task<T>`，使用 `CO2_BEG` / `CO2_AWAIT_SET`；停止来自 promise 的 `co2::stop_token`。
  常规公开 I/O 返回可 co_await 的 awaiter，而非一律 Task。错误用 `uvco::Result<T>` / `Errc`。
- `Timer::create/start(milliseconds, repeat=0)/tick()/next()/stop()` 已存在；tick 返回计数，不是时间戳。
  TimerBlock 为 final；不会为了 Session 继承它或把可靠包塞进 watcher DeliveryPolicy。
- 复用 `detail::HandleBlock` / `HandleRef` / `WaitSlot<T>` / `CancelHook`。
  WaitSlot 预约保留到 take，要求 Result payload nothrow move；槽本身不持有 owner、不决定取消或恢复策略。
- Loop schedule 默认每队列65536项、构造期 reserve，批次 FIFO；它不是严格 next-turn，也不保证实时 deadline。
  KCP 必须合并内部唤醒、限制活跃 waiter/endpoint 数，不能每 output 包排一个 continuation。
  `~Loop` 禁止存活的外部 KeepAlive，resident/native handle 必须能取消并 drain。

## 2. 分层与方案选择

```text
业务协程：sendMessage / recvMessage / drain / close
                  │ Result、stop_token、预算
KcpSession ─ SessionBlock（稳定地址、WaitSlot、HandleRef、timer driver）
                  │ 同步调用 + 显式 now32
KcpCore ───────── ikcp.c/h 原样、RAII、无 I/O/调度
     │ output(data,len)：只 tryCopy + 计数/错误标志
     ▼
BoundedPacketQueue（拥有完整数据报；全局池 + 每 session 限额）
     │ 退出协议调用栈后，endpoint TX pump 才消费
UdpEndpoint ─ 一个 recv pump / 有限 TX jobs / map[(peer,conv)]
     │                      ▲
现有 uvco::Udp + Loop        单调时钟 / check 驱动的 one-shot timer
```

推荐 **共享 endpoint + 每 Session 一个 one-shot driver**：先复用 Udp 收发与 Timer backend 的真实句柄模式，
不要第一版做通用协议调度框架。可单独创建 endpoint 供一个 Session 使用，也能显式共享。

备选：共享最小堆/一个 timer，减少大量会话的句柄和空闲唤醒，但 generation/删除/公平性更复杂，
待规模测试证明瓶颈后迁移；独立协议线程会引入跨线程复制/取消/保活，首版不采用。
此分层让将来更换 timer scheduler 不影响 Core 或业务完成语义。

拟定目录：`third_party/kcp/`（原始文件、LICENSE、版本说明）、`include/uvco/kcp/{core,session,endpoint}.hpp`、
`include/uvco/kcp.hpp`。不扩充 watcher DeliveryPolicy，不修改通用 UDP 的零缓冲背压语义。

## 3. KcpCore：纯同步、可模拟的协议核

接口草图（**拟议新 API**，不是已存在/可编译承诺）：

```cpp
using KcpMessage = std::vector<char>;
struct KcpOutputSink {
    void* context;
    bool (*tryCopy)(void*, ConstBuffer) noexcept; // 只能复制到有界队列
};
class KcpCore { // move-only，稳定 pImpl；搬动 owner 不改变 ikcpcb.user
public:
    static Result<KcpCore> create(std::uint32_t conv, CoreOptions, KcpOutputSink);
    Result<std::size_t> sendMessage(ConstBuffer); // V2 成功返回接受字节数，非恒等0
    Result<void> input(ConstBuffer datagram);
    void update(std::uint32_t nowMs);
    std::uint32_t check(std::uint32_t nowMs) const; // 绝对32位时间，不读 clock
    void flush();                               // 不刷新时间，必须先 update
    Result<std::size_t> peekMessageSize() const; // 不完整 -> again
    Result<KcpMessage> recvMessage();            // 一条完整消息；空消息不是 EOF
    std::size_t waitingSegments() const;         // snd_queue + snd_buf，非字节
    bool deadLink() const;
    std::uint64_t outputDrops() const;
};
```

- `CoreOptions` 只含 MTU、窗口、nodelay、重传等协议参数；不接收 Loop、socket、clock 或 callable 计时器。
  output sink 的依赖契约是无 I/O 的 queue writer；生产队列在创建时预分配，模拟测试也用相同接口。
- Core 采用稳址 pImpl，output user 指向 pImpl，sink context 的生存期必须覆盖 Core；禁止回调中 destroy/input/flush。
  不公开 mutable raw ikcpcb；更新固定版本时只审计这个边界。
- 消息模式预检 `ceil(length/MSS) < 128`，也检查 int 长度范围。该版本最多127段，不能按255段假设。
  MTU1200 时 MSS1176，协议单次上限149352字节；配置更小 MTU 时必须重算上限，不能只看64KiB应用限制。
- `ikcp_send` 的失败/部分接受不保证回滚。正常消息要求接受量等于原长（零长也合法）；
  调用后异常返回或短接受将 Core 标记为不可再提交，Session 终止，禁止整包重试导致重复前缀。
  数字返回码需按固定源码逐路径审计，不把所有负值或 -2 随意映射成 ENOMEM。
- input 可能处理前半包后才报错；外层先完整预检所有24字节段头、conv/cmd/len/frg 与边界，
  然后调用 Core。预检失败整包丢弃；预检通过仍遇 native input 失败，则保守终止该 Session，避免复用不确定状态。
- peek 后分配消息 buffer；C++ 分配失败返回 no_memory，不消费接收消息。Core 不自行读取时钟或运行协程。

**OOM 不能虚报保证**：固定上游部分 C 分配路径会 assert/解引用失败指针，未提供完整可恢复错误；
不修改 vendor 与“所有 OOM 都变成 Result”不能同时直接保证。队列/窗口预算仅降低风险，不能解决系统 OOM。
不在单个 Session 中替换全局 `ikcp_allocator`；若要使用专门 allocator，必须在任何 Core 创建前确定全局策略。
实施第一步必须审计并记录此限制；不满足用户要求就停在审批点，不私改 vendor 或谎称 catch 能捕获 C abort。见§12。

## 4. KcpSession 协程契约

```cpp
class KcpSession { // move-only，绑定单个 Loop/endpoint/(peer,conv)
public:
    SendMessageAwaiter sendMessage(KcpMessage message); // await -> Result<void>
    RecvMessageAwaiter recvMessage();                   // await -> Result<KcpMessage>
    DrainAwaiter drain();                              // await -> Result<void>
    CloseAwaiter close();                              // await 本地关闭，不是网络 FIN
    void abort(Errc reason = Errc::aborted);             // loop线程，幂等
};
// 使用已有 co2 模式，不另造 Task/stop-token 系统：
// CO2_AWAIT_SET(result, session.sendMessage(std::move(message)));
// CO2_AWAIT_SET(messageResult, session.recvMessage());
```

awaiter 名为草图，不直接冒充已有 detail::CloseAwaiter 的全部行为。通过 endpoint 的显式 openSession 工厂创建，
返回 `Result<KcpSession>`。停止 token 从 promise 取得；若以后增加显式 token 重载，只作同一取消逻辑的适配。
公共方法调用时就捕获 `HandleRef<SessionBlock>`，**不能等 lazy Task 开始执行才借用 facade**；
如用 Task 外壳，须在普通工厂函数先取得 ref，再按值交给 coroutine frame。

### 4.1 完成、并发与取消

| 操作 | 成功与并发 | 取消 |
|---|---|---|
| sendMessage | 整条消息进入本地 KCP queue 即成功；非 UDP send 完成、非远端 ACK、非远端消费。v1 一个待 admission sender，第二个 busy；已提交消息可有多条 | 提交前 canceled 保证未进入 KCP。提交后至多取消尚存在的等待，不撤销可靠段；本接口通常已在提交点完成，迟到 stop 不覆盖已锁定成功 |
| recvMessage | 一次一条完整消息；**单 receiver**，第二个（包括首个已完成但未 take）返回 Errc::busy，不触发通用 WaitSlot 的并发契约 abort | 完成前取消只撤销 waiter，不消费消息、不停止共享 UDP；已完成结果不被晚到取消覆盖 |
| drain | 在某个 loop 决策点同时满足 waitsnd==0、本 Session tx queue==0、native TX in-flight==0；仅 ACK/本地输出空闲，非远端业务已处理 | 只取消 drain waiter，不撤销已发送消息、不关 Session；v1 单 drain waiter，第二个 busy |

并发新发送可延长 drain，甚至使其一直等；它不是某条消息的 ACK future 或固定发送屏障。
未提交的 send waiter 不计入已发送集合，drain 返回后仍可能提交；调用者要稳定屏障须先停止并发 producer。
`sendMessage(KcpMessage)` 在调用时转移 buffer 所有权；即使取消/拒绝也不保证退还 buffer，需要重试的调用者保留源副本。
库限制内部待 admission 数量；调用者自己囤积的未启动 awaiter/消息不属于库的内存预算。

决策优先级：已终止 -> terminal error；仍活跃但预取消 -> canceled；槽被占 -> busy；然后尝试数据/预算。
每个操作只完成一次。send 在整条提交点、recv 在结果所有权取走点、drain 在空闲条件成立点锁定结果。
所有业务通知均 `loop.schedule` 延后，**包括 native callback、取消 callback 与关闭**；
不得在 input/update/flush/output 栈上恢复业务协程。与旧模块 native inline 策略的差异是本模块防重入的明确选择。

### 4.2 错误模型

沿用现有 Errc，不把 KCP 自有负数直接当 libuv 错误：

- 预取消/等待取消：`Errc::canceled`；显式 close / ~Loop：`Errc::aborted`。
- 接收/发送 admission/drain 冲突、会话/accept/注册限额：`Errc::busy`。
- 非法选项/conv 注册参数：`Errc::invalid_argument`；重复 `(peer,conv)`：`Errc::address_in_use`。
- 超消息上限：`static_cast<Errc>(UV_EMSGSIZE)`；协议不可继续：`static_cast<Errc>(UV_EPROTO)`。
- 可恢复分配失败：`Errc::no_memory`（不承诺捕获上游不可恢复 OOM）。
- dead_link / 配置的等待期限到期：`Errc::timed_out`，统计分别记录 deadLink / deadline 原因；
  本地持续拥塞终止：`static_cast<Errc>(UV_ENOBUFS)`；socket 错误保留对应 Errc。
- 正常 UDP 截断、未知 conv、来源不匹配、预检坏包：丢弃并限频计数，不把单个不可信坏包当作全 endpoint 错误。
  Core 再无消息的同步查询用 again；异步 recvMessage 不靠 again 忙轮询。没有隐式 EOF，零长消息合法。

终止原因首次锁定并广播给尚 pending 的操作；此前已完成/canceled 的槽不被后续 close 改写。

## 5. 背压与所有队列的界限

以下为首版建议值（预算参数可调，尚非已发布默认）：

| 预算 | 起点 / 规则 |
|---|---|
| 消息/MTU | 应用消息64KiB、KCP MTU1200；还受127*MSS限制；MTU必须扣除未来认证/加密/FEC开销 |
| KCP 发送水位 | 段数 high256 / low128；最大512段；保守字节水位 high512KiB / low256KiB / hard1MiB。配置须允许至少一条最大消息，不可永远等不到 admission |
| admission waiter | 1条 owned 消息；第二个 busy，不创建无界 Task 或 waiter 链 |
| 接收 | 不额外搬运到无界应用消息队列；无接收者就留在 rcv_queue，收窗自然变小。rcv_queue/rcv_buf、ACK列表也计入内存审计 |
| output | 每 Session 最多256个完整数据报，endpoint 全局2048个预分配 MTU-sized slot；可共享池，创建/配置时核对总字节预算 |
| UDP in-flight | endpoint 最多32个 native send，每个拥有 buffer + Session/Endpoint 引用；含在全局 packet 预算中 |
| endpoint | 建议最多256个 Session、16个 pending accept；CPU每次调度的 input/发送处理量也设预算 |

`waitsnd` 包含已发未 ACK 段；不能仅以 snd_queue 是否为空判断可写。
Core 统计快照给出保守 outstanding payload/元数据占用；如无法精确按 ACK 释放字节，可用
`waitsnd * MTU + 已知固定开销` 的保守预算，不能以低估内存换吞吐。
发送到达 high 后阻塞，只有同时低于段/字节 low 才重新开放；提交前再检查整条消息后的 hard limit，
禁止先提交一半再挂起。ACK/input、recv 释放窗口、timer 驱动后都重新判断水位。
窗口不是应用排队硬上限；不要删除已分配 sn 的段来丢“过期帧”。

**output 满队列策略**：默认丢新数据报（整包，不丢包内某段）、增加 per-session/global drop 计数，
返回失败供统计但不依赖 ikcp_flush 传播错误。已接受的可靠消息仍留 KCP 等重传，ACK 丢失由对端重传补偿。
回调只设置 queue-full 标志；时间、告警、发送唤醒、终止判断都由调用者退出 Core 后处理。
不为 ACK 冒充可无损回压；PUSH/ACK 可混装，首版不在 output 内做复杂优先级解析。

endpoint TX pump 按 Session 轮转，不能让一个 conv 占满所有 native 请求。单次满不终止；
持续本地队列满建议以2s观察窗、累计至少1024丢弃作为可调保护阈值，触发 ENOBUFS 终止该 Session，
不能永久静默丢包或无限重试。普通 UDP EAGAIN 在有限队列内等待/受限重试；永久 socket 错误按实际范围终止会话或 endpoint。
output 借用 buffer 在回调返回后即可失效，native send 必须持有复制件到完成。

## 6. 时间、精确驱动与低延迟

1. outer driver 读取 `steady_clock` 的单调64位毫秒，取低32位传 Core，先 `update(now32)` 再 input，
   保证 ACK RTT 样本不使用陈旧 current。初始化也必须 update；flush 首次 update 前无效。
2. input 后根据 `immediateFlushAfterInput` 可主动 flush，再评估可读/可写/dead_link、check；
   flush **不只发 ACK，也可能发数据/重传**。建议默认开，每个有限 input batch 至多一次额外 flush；
   batch 不能为省调用长时间攒包（设数据报数与时间预算），快 ACK 的收益用尾延迟验证。
3. send 提交后也可立即 flush（时间已更新），不让消息平白等20ms；recv 消费导致窗口恢复后重新驱动。
   output 只是队列复制；调用结束后才安排 endpoint TX pump，合并每 session/endpoint 的调度节点。
4. `check(now32)` 是绝对时间。用定义良好的32位模差还原 signed delta（不依赖实现定义的窄化转换），
   映射成 `now64 + max(delta,0)` 的本地 deadline；只比较小于2^31ms的跨度。
   约49.7天正常回绕可继续；外层记录64位 last-update，暂停达到2^31ms时不复用旧比较状态，
   保守 timed_out 并重新建立会话，不把陈旧 ts 当遥远未来；挂起/恢复时钟行为也需测试。
5. 用 one-shot Timer/相同 Timer backend 预约最近 deadline，不设固定 repeating20ms心跳。
   新 send/input/窗口变化使旧假设失效，重算并提前 rearm；没有更早事件时避免不必要重置。
   毫秒向上取整，不早于 deadline；记录 timer lateness 和 schedule queue 等待时间。
6. **check()==now 防忙转**：一次事件只做一次 due 驱动；update 后仍 due 时允许一次显式 flush，
   再 check。仍无状态推进则最早延后1ms；若连续出现同一无进展 due 状态，退到下一 interval/刷新边界，
   不能无限预约0ms timer或反复 schedule 自己。这个保护不把所有正常重传强制退回20ms；
   测试必须同时证明无忙循环与 ACK/重传 deadline 的尾延迟预算。

check 会扫描 snd_buf，而且当前版本通常最多等 interval，即使空闲也可能周期唤醒。
首版不擅自关闭空闲 timer：只有证明无待发/未确认/ACK/窗口通知/零窗探测等状态后才能优化；
CC 插件以后可能另有 tick 要求。不能声称 check 自动实现 O(1)/零空闲成本。
Turbo 仅用于受控网络/额外整形后显式选择，统计重传、带宽和与 TCP 的竞争公平性；不开放直接改 rx_minrto 的“神奇低延迟”默认。

## 7. 生存期与关闭：不是 fake handle，也不产生脱离 owner 的 Task

- `SessionBlock : HandleBlock` 拥有真实 `uv_timer_t` 作为 resident/native anchor，复用 Timer backend/一次性驱动规则，
  不是继承 final TimerBlock，也不在无 native handle 的对象上调用 bindHandle/uv_close。
  Core/peer/conv 稳址；send/recv/drain awaiter、排队 drive job、TX job 均持有 HandleRef。
- endpoint 复用现有 Udp owner（不篡改 UdpBlock final/data）。建议 `EndpointBlock : HandleBlock` 另持真实
  `uv_async_t` 生命周期 anchor，并拥有 Udp；接收/TX pump 由它结构化启动、取消并 drain，
  native ref 只覆盖内部工作存活期。新增 anchor 是为稳址/Loop-first 清理，不充当另一个用户态调度器。
- 不把外部 Loop::KeepAlive 跨 Session/endpoint 生存期保存。活跃 endpoint recv / driver timer 会保活；
  `uvco::drain(loop)` 与 `session.drain()` 完全不同，前者需应用先关闭活跃 endpoint/Session 才能空闲。
- 复用 WaitSlot<void>/WaitSlot<KcpMessage>，模块先检查 idle 再返回 busy，不改变 WaitSlot 本身的契约。
  HandleRef 自构造持有；opBegin/opEnd 只计真正挂起，最后释放后不得再访问 this。

本地状态：Active → Stopping（锁定原因）→ native Closing → Closed；generation 只用于丢弃旧工作，不是线上认证。
**close/abort/析构/~Loop 都走同一停止入口，且同时覆盖 beginClose 与 loopClosing**：

1. 锁定 stopping/原因并增加 generation，禁止新 admission/input/输出；从 endpoint demux 中移除该注册。
2. stop driver timer，撤销/失效化排队 drive job。若当前仍在 Core 调用栈，只记 terminal 标志，退出后再清理，
   绝不在 output 中销毁 ikcpcb。
3. pending send/recv/drain 各锁定一次错误，经 loop.schedule 恢复；清除尚未交给 native 的本 Session 数据报。
4. **共享 endpoint 继续服务其他 conv**：单 Session 关闭不能 cancel 共享 recv 或 close UDP。
   已提交 Udp::send 无法单独取消，保留 buffer/ref 并等待回调；不能用 whenAny 提前销毁 send awaiter。
5. 等该 Session 的 Core 栈/内部 job/TX in-flight 安全归零后 release Core，最后 close timer anchor。
   endpoint 自身关闭则先拒绝全部注册、abort各 Session、取消接收泵、关 Udp、drain所有 native send/pump，最后关闭 async anchor。
   逻辑 stopping 与物理 close 分开；不得先关闭 anchor 让未完成内部 Task 失去保活。
6. close awaitable 等内部 I/O fence/native close 完成，不承诺其他用户已 take 结果；这些旧 awaiter 的 HandleRef
   仍可保活控制块。单纯 Base CloseAwaiter 不等待所有 holds，必须由派生控制块把内部 fence 放在 native close **之前**。
   ~Loop 驱动这些清理，不在析构中等待网络 ACK；孤儿 owner 之后只可 close/析构。

close 是本地立即停止，不发 FIN、不可靠通知对端；要尽力发送已提交消息，应用先带 deadline 等 drain 再 close。
析构沿用现有 Handle 规则：带活跃用户挂起操作直接析构是契约违规，先 cancel/close 并等待 Task 完成；
不能靠 ref 就声称可以任意销毁仍在 await 的 coroutine frame。
已到内核的数据报无法撤回，close 返回也不保证对端不会收到此前报文；不立即复用同一 peer/conv 以冒充新会话。

## 8. 多 conv 的共享 endpoint 与显式接入

键为 `(normalized peer address+port+IPv6 scope, conv)`，不是单独 conv；每 endpoint 只有一个 recv pump。
不开放 NAT 重绑定/迁移；陌生地址+相同 conv 不接管旧会话。conv 不提供身份、加密、握手或防重放。

```cpp
// 拟议：显式注册已协商的会话，返回 Result<KcpSession>
endpoint.openSession(peer, conv, SessionOptions{});
// 备选 pull 接入：仅返回有界 PendingPeer/OwnedDatagram 元数据
CO2_AWAIT_SET(candidate, endpoint.acceptPeer());
// 应用完成限流/鉴权/协商后显式 openSession；未知包本身从不自动创建 Core
```

- 先确认 Datagram 非 truncated、长度至少24，再解析 little-endian conv；不能对不足4字节的数据调用 getconv。
  校验整个聚合数据报属于同一 conv、cmd 合法、len 不越界；不把一包当一条消息。
- 默认未知 conv 直接丢弃、计数；**明确启用 accept 才收集候选**，候选池有条目/字节/每 peer/速率/TTL 上限。
  候选不分配 Core，推荐只留首个受限数据报/元数据，其余丢弃；openSession 成功且代次/TTL仍匹配后才能显式重放该包。
- 若选 callback 接入，callback 只向有界候选队列投递，noexcept、不 await，不在原生 recv/Core 栈里执行应用握手。
  不能声称一个裸 callback 就有安全 accept；最终必须显式注册且有 endpoint 全局/每 peer session 上限。
- 复用/关闭 generation 防本地旧队列命中新对象，但不防线上旧包重放；conv 由上层协商并避免危险复用。
  认证/加密/握手/反射放大保护归上层，首版不能给不可信公网“开箱即安全”的承诺。

## 9. 构建、安装与依赖

已确认 `UVCO_ENABLE_KCP=OFF`，不默认引入 C 对象或改变现有 header-only 基础 target。
开启时构建原始 ikcp.c 的静态 C target，C++ wrapper/Session 作为可选 `uvco::kcp` target，链接基础 `uvco::uvco`；
vendor C 源码不调用 libuv；当前合并的 uvco::kcp target 通过基础 uvco::uvco 传递既有依赖，暂不另导出独立无libuv的Core包。KcpCore 为复用 Result/Errc 可包含现有错误类型头，但不能引入 Loop/I/O 或 runtime 时钟依赖。
不为“纯核”另外复制一套 Result；纯核测试不创建 Loop/UDP/timer，并用 syscall/依赖审计验证隔离。

vendor 原文与上游 LICENSE 一起保存，版本文件记录 commit/来源/文件 SHA256；实现时核对本 pin 的原始 notice。
不修改 ikcp.c/h、不在消费者 configure 时自动追 master；全局 allocator/编译宏/CC 扩展的约束写入版本说明。
开启才启用所需 C 构建；POSITION_INDEPENDENT_CODE/平台 C 编译与链接依赖在真实目标编译验证，不能猜测。

安装保持 GNUInstallDirs：导出 `uvco::kcp` 和它需要的 C target、wrapper/vendor headers 与上游 MIT notice；
基础 uvco license 与 KCP notice 分开，不用自己的 MIT 文件替代上游版权。
Config/Version、libuv/co2 find_dependency 和能力探测沿用现有逻辑；设计增加 kcp component 能力信息，
安装包未启用 KCP 而消费者请求该 component 时明确 not-found，不自动下载或静默退化。
源码 add_subdirectory/FetchContent 与 installed consumer、前缀搬迁、两个 TU 都测试 ON/OFF；
OFF 包含基础 umbrella 不要求 ikcp.h；KCP 专属头的不可用需有明确诊断。以上都只是后续实现计划，本次不改 CMake。

## 10. 测试：RED-first，先证明协议，再接 I/O

每项先写可重复失败回归，再提交对应最小实现，不以真实 sleep 或公网作为协议正确性的判据。

1. **固定依赖/纯核**：文件字节与 SHA256、MIT、V2正接受字节数；移动稳址/销毁/output不得重入。
   模拟64位时钟取低32位，有损链路按 seed 生成丢失/乱序/重复/延迟；同 seed 输出轨迹一致。
2. **协议性质**：可靠有序、无重复交付、聚合/分片、零长、127段/128段边界；尾包/ACK丢失、burst loss、
   零窗恢复、窗口不足、大消息上限。断网/dead_link 不靠输入事件也能 timer 超时。
3. **时间**：跨0xffffffff、差值边界、长暂停；check 返回 now 而周期 flush 尚未到期；
   检查最大驱动次数、无0ms忙转、发送/ACK/重传延迟，不能只验证不崩溃。
4. **预算**：高/低水位滞回、一个 admission waiter/第二个busy；慢接收者、不读取时窗口收缩；
   output/tX pool满、持续满终止、并发session公平；绝不无限Task/数据队列；借用output buffer复用后 native发送仍正确。
5. **协程/生存期**：预取消未提交、提交后取消不撤段、drain取消/并发send；单 receiver冲突、done未take冲突；
   close/abort与input/flush/output/timer/TX完成交错；构造await后移动/关owner、~Loop先销毁、共享endpoint单session关闭不伤其他会话。
   事件轨迹中每 waiter 完成一次、内部fence后无访问Core/旧代次，外部Task必须可结构化结束。
6. **分流/恶意输入**：peer+conv碰撞、未知conv洪水不建Core、候选限额/TTL、截断、坏len/cmd/混conv整包拒绝；
   sender address不匹配、conv复用、socket失败、native send不可单独取消的清理。OOM可恢复路径注入与不可恢复限制分别记录。
7. **集成/包**：本机 UDP loopback 双端；g++/clang++、ASan+UBSan（leak）、TSan 四构建重复；
   Header自足、ON/OFF、安装/搬迁/consumer/两TU，fd/handle泄漏；不因为 experimental 平台就宣称已支持。

## 11. 性能验收：验证 30–40% / 3x，而不是引用成结论

Release 构建测完整**应用消息 echo RTT**，不是 UDP send 耗时或 drain/ACK 时间。
TCP 对照做相同长度 framing，测试 TCP_NODELAY 开/关，启用系统正常 SACK，不用过时 TCP 假设制造优势。
同一应用负载/路径/CPU绑定，分别比较 KCP默认、fast ACK关/开、turbo、TCP；ASan/TSan只查正确性，不作性能结论。

矩阵：RTT10/50/100/200ms、jitter0/10/30ms、loss0/0.1/1/5/10%、burst/尾包/ACK丢失、乱序/重复、
带宽1/10/100Mbps；消息128B/1KiB/16KiB/64KiB，单/多会话、慢消费者与背景竞争流。
模拟器先覆盖可重复轨迹；真实网络整形仅在获准的隔离 namespace/container 环境，不能为跑测试修改用户主机网络策略。

每场景至少5个 seed/重复，预热后测60–300s；报告 p50/p90/p95/p99/p99.9、max、超时/失败率、
goodput、线上总字节/重传/ACK占比、CPU、RSS、队列峰值/丢弃、timer lateness、schedule queue delay。
超时不能从分位数样本悄悄删掉，单列删失样本并报告固定窗口内 max；提供置信区间和原始配置。

平均 RTT 降幅计算 `(TCP_mean-KCP_mean)/TCP_mean`，检验是否30–40%；“3x”分别给出
`TCP_p99/KCP_p99` 和 `TCP_max/KCP_max`，明确是否达到3，不能偷换成吞吐3倍或任何网络的硬上界。
记录带宽代价是否约10–20%、nc=1的公平性代价；无丢包或竞争网络可能更差也如实报告。
验收包括低尾延迟与可接受资源/公平性，不以一个平均值掩盖忙转或最大延迟退化。

## 12. 已确认决策（2026-11-08，Asia/Shanghai）

1. UVCO_ENABLE_KCP 默认 OFF；启用后显式链接 uvco::kcp，支持安装/导出。
2. 首版包含 stream 模式，使用独立 KcpStream；不与 KcpSession 的消息接口混用。
3. 未知 conv 使用 pull 式 acceptPeer，有界候选，绝不自动创建会话。
4. 默认应用消息上限64KiB、KCP MTU1200；有效上限 min(config,127*MSS)，可配置。
5. input 后即时 flush 默认 ON；每个有限 batch 至多一次，用尾延迟/CPU基准验证。
6. 接受原样 vendor 的部分 OOM 不可恢复限制，文档明确边界。

### 12.1 首版 KcpStream（与消息 API 隔离）

KcpStream 在创建 Core 时设 stream 模式，之后不允许切换；公共工厂只返回一种 facade。
不暴露 recvMessage/sendMessage，不把一次 write 映射成远端消息边界。
拟议 write(ConstBuffer)、readSome(MutableBuffer)、drain()/close() 与现有流命名对齐；
write 成功返回已进入本地可靠队列的字节数（可短写），不表示整份输入已接纳或对端消费。单 pending writer/reader，第二个返回 busy。
write 在进入 KCP 前复制并检查预算；KCP可能短接受，已接受前缀不可回滚，
不把失败后的整段输入再提交。发送前取消保证零提交；发生部分提交后的取消仅取消等待，不撤销前缀，
API必须保留已接受字节数（Result<size_t> 成功可为短写），与消息模式的整条 admission 分开。

ikcp_recv 取完整内部单元，不能拿很小用户 buffer 直接反复调用：readSome 先取一个完整单元至
有界 leftover buffer，再复制 min(用户容量,剩余长度)，保留余量供下次读取。
只维护一个 leftover，不无界搬运 rcv_queue；该 buffer计入接收预算。取消发生在消费前不拿走数据，
已完成 read 的迟到取消不覆盖结果；空 buffer请求返回0，不借此宣称网络 EOF。
KCP没有 FIN；对端 close 不会自然导致本端 EOF。显式本地 close/abort、dead_link 和超时按Session错误模型处理。
混合 message/stream peer不做自动协商或兼容，创建时由应用外部协议约定。

## 13. 审批后的实施顺序

A. 固定 vendor/版权与分配失败审计（OOM要求不相容则先停下审批），可选构建骨架与纯核 RED 测试。
B. KcpCore/模拟时钟与有损链路，把返回值、片数、回绕、dead_link 的行为先钉牢。
C. bounded output pool + 现有 Udp endpoint 收发泵，明确请求缓冲和有限 TX jobs。
D. Session 稳址控制块/one-shot driver、send/recv/drain、背压/stop-token 与 close/~Loop 轨迹测试。
E. 多peer/conv显式注册与有界 accept，ON/OFF安装/consumer/双TU和四构建重复。
F. Release RTT/最大延迟/资源/公平性基准；只有证据支持才做共享 timer 或更激进预设。

每步独立提交与回归；默认构建仍关闭 KCP，任何超出已批准语义的改变先报告审批。

### 固定来源

- [上游 README](https://github.com/skywind3000/kcp/blob/b1a7a2101dcbb96017681a500d6b82bbe5a88766/README.md)
- [ikcp.h](https://github.com/skywind3000/kcp/blob/b1a7a2101dcbb96017681a500d6b82bbe5a88766/ikcp.h)
- [ikcp.c](https://github.com/skywind3000/kcp/blob/b1a7a2101dcbb96017681a500d6b82bbe5a88766/ikcp.c)
- [现有架构](architecture.md)、[统一 watcher 设计](design-watchers.md)、[验证方法](verification.md)

## 14. 首轮实现进度与审批记录（历史，已由§15更新）

已分步提交默认 OFF 的 vendor/target、纯核、Session、独立 Stream、共享 endpoint 与 pull accept。
当前真实类型仍为 `Udp`；Session 使用相同 TimerBackend 的真实 one-shot timer anchor。
endpoint 使用预分配 packet pool，公平轮转会话，首版 TX pump **串行、最多1个 native send**，
不是设计预算上限32个并发。段数高/低/硬水位已经实现，字节占用可由段数×MTU保守估算；
独立字节水位和持续 output-full 的保护阈值尚未完成。勿把提议值当作现有选项。

输入默认即时 flush 已实现，但 input+sender admission 同轮的额外 flush 合并仍需收敛；
超上限接收消息的终止路径、Loop-first/pending close/多waiter及候选TTL回归仍需补强。
上述是后续审查事项，不是已通过的完整验收结论。安装基础consumer已通过，KCP专用consumer/双TU待补。
benchmark与Linux KCP ON CI尚未实现；基础CI未改变，默认KCP仍OFF。

### 必须先确认的 vendor 编译适配

对 vendored C 也开启 ASan/UBSan 后，四项KCP测试稳定报错：

```text
ikcp.c:1081: member access within null pointer of type IKCPSEG
IOFFSETOF(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER) // ikcp.h:154
```

这是 container-of 所依赖的旧式 offsetof 宏引起的 UB 检查，非用户代码把实际 segment 传成 null。
它不属于已接受的“部分分配失败不可恢复”，也不能通过不检测 vendor 或放宽 UBSan 假装全绿。
原始文件的固定 SHA256 检查通过；目前没有改 vendor、加 suppression 或切换版本。

**待审批问题**：是否允许新增 uvco 自己的 C 编译适配 TU，先包含原始 ikcp.h，
将 IOFFSETOF 显式重定义为标准 offsetof，再包含原始 ikcp.c 来编译？
原始 vendor 文件仍逐字节不变，ASan/UBSan继续检测全部代码；这改变有效宏定义，故不擅自当作“原样编译”。
另一选择是审批最小上游补丁或其他版本；不建议通过关闭 null sanitizer 绕过。

当时依据“超出已批准语义先停下报告”的要求暂停；§14.1记录批准，最新实现状态见§15。

### 14.1 编译适配已获批准

2026-11-08（Asia/Shanghai）用户批准 `src/kcp_adapter.c`：包含原始头文件，
将 IOFFSETOF 重定义为标准 offsetof，再包含原始 C 文件。仅改变该 TU 的有效宏，
vendor 文件/版本/SHA256 全部不变；不关闭任何 sanitizer。原§14审批阻塞已解除。

## 15. 后续实现与最终契约

- `src/kcp_adapter.c` 按批准方式适配标准 offsetof，vendor c/h/LICENSE 与固定 SHA256 未变。
  C/C++ 都完整开启相应 sanitizer，没有 vendor 例外/suppression。
- driver 收集 input ACK、send admission、recv释放窗口的 flush 请求；退出 Core 调用后每个驱动 batch
  **最多一次显式 flush**。`update` 自身仍可按 KCP 时间表 flush；没有把这个次数伪装成所有 native flush 的次数。
  `KcpSession/KcpStream::stats()` 提供 inputBatches/admissions/explicitFlushes/maxExplicitFlushesPerDrive、
  outputDrops、queuedPackets、waitingSegments，均在绑定 Loop 线程读取。
- 段水位默认 high256/low128/hard512；保守字节水位 high512KiB/low256KiB/hard1MiB，
  `KcpSessionOptions::{byteHighWater,byteLowWater,byteHardLimit}` 可调。按段数×MTU估算发送占用，
  不是整个进程RSS/接收buffer/共享pool的总上限；工厂验证至少容纳一条最大消息，避免永远无法 admission。
- output满则整包丢新、可靠段留待重传。`congestionDurationMs=2000` 与 `congestionDropLimit=1024`
  **同时满足**才以 ENOBUFS 终止持续本地拥塞；一旦队列有空间即重置观察窗。纯保护器使用注入时间测试。
- 过大消息输入终止 Session 并保留 EMSGSIZE 原因，不让无法取走的 queue head 永久卡住。
  stream接收单元与消息 admission 上限分开；leftover最多一个 native单元（受MTU约束），不是消息队列。
- 用户挂起的 send/recv/read/drain/accept 完成经 schedule queue；KCP Session/Endpoint 的 **native close
  完成也延后**。HandleBlock增加 opt-in CloseResumePolicy，其他模块默认 inlineNative 完全不变。
  预取消/槽冲突/已关闭等同步拒绝仍可在 await_suspend 返回 false，空/已关闭 close 同样可立即完成。
- Linux lifetime测试强制 uv_udp_try_send 返回 EAGAIN，覆盖真正 native请求在途时 ~Loop 先销毁、
  本地已提交 send 成功不被后续关闭改写；另覆盖 done未take、关闭pending、共享conv隔离和重复fd计数。
- accept覆盖过期、错误endpoint、已使用提案重放、取消/关闭/busy，以及独立全局/每peer候选上限。
- KCP包验证 installed+relocated/add_subdirectory/FetchContent 与两TU链接；OFF包拒绝 required kcp。
  header-only OFF版本保留架构无关，含静态C对象的ON包检查架构。上游MIT独立安装并校验SHA256。
- Linux CI所有现有矩阵启用KCP（含fetched-deps），同时传 CMAKE_C_FLAGS / CMAKE_CXX_FLAGS 的sanitizer flags。
  benchmark独立开关、不进CI时延门禁；macOS/Windows仍属原有非阻塞范围，KCP原生移植不额外宣称通过。

### 15.1 仍保守保留的设计选择

endpoint TX目前公平轮转会话、单 native send（可同步 fast-path），未扩成32路请求并发；
这是有界预算内的首版实现，不改变发送完成/可靠性语义。规模化共享timer heap、FEC/加密/握手/FIN、
路径迁移与公网安全接入仍不属于当前实现。部分vendor OOM不可恢复的已批准限制继续存在。
本机loopback结果不能证明公网30–40%或3x优势；复现命令和数字见 [KCP基准](benchmark-kcp.md)。

接收窗口参数限定128..65535，避免16位wire窗口通告溢出为零；stream write在复制用户buffer前
先检查sticky terminal、预取消和busy，保持与消息接口相同的同步拒绝优先级。
