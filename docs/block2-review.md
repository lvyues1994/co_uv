# Block 2 全量自查与交付记录

范围：`461b27d..HEAD`（watcher core、FsEvent/FsPoll/Timer、Signal、phase、TTY、Resolver、HE、POSIX group）。
这是本地 fresh-eyes 自查，不是另一个独立 reviewer 的审查；没有更改五项批准默认值。
工作仅在 co_uv 内，临时材料在 build/，没有 push、没有 .vscode 改动。

## 架构判断

- 保留三个独立维度：typed Backend、EventPort/HookSink、编译期 DeliveryPolicy；没有大而全的虚 Watcher。
  WaitSlot 预约到 take、构造期 HandleRef、native/control/deferred resume 规则仍由原有基础层控制。
- FsEvent/FsPoll 的 FIFO32、合并/溢出及 Timer legacy 的累计/零 wrap/关闭优先级不变；
  流的背压/缓冲所有权、UDP datagram、DNS 单次请求、Process sticky exit 不应硬塞 watcher。
- Hook 持有 noexcept callable、无 std::function/每事件分配；Signal 多订阅计数与全局 disposition 分开。
  Phase backend 很薄；高级 next 延后，不伪装精确 phase continuation 或 next-turn。
- TTY 只共享 StreamBlock，不继承暴露 accept/listen 的公共 facade；fd guard/restore 覆盖双入口。
- Resolver 拥有独立 producer、immutable shared result、精确 epoch 与有界线性 LRU（默认256项/32 flights）。
  native async ref 解决 KeepAlive 与 ~Loop 的冲突，不削弱外部 KeepAlive 契约。
- HE 独立 first-success coordinator，而非 whenAny(connectA,connectB)；拆开候选选择、解析、worker、guard。
  每连接 task/slot、候选 owned copy 有分配，换取稳定所有权；不为有限并发引入 arena 或另一个调度框架。
- 检查了 ODR/头文件自足、co2 compatibility 边界、cancel/close/Loop-first、callback 后无 this 访问、
  owned callback 重入、native PID 身份与测试 fixture 自行回收后代。没有发现更多已支持契约内的阻塞问题。

## 发现并修复的真实问题

下列位置是修复后的保护点；RED 日志均位于 `build/block2-cont/`。

| 严重性 / 问题 | 证据与修复 | 回归 / 提交 |
|---|---|---|
| P0：capture 用 kill(pid,0) 当作 PGID 身份，漏掉 libuv reap-before-exit-callback 窗口 | `process/capture.hpp:33` 改调 `Process::signalGroup`；`process/process.hpp:205` 先 waitid(WNOWAIT)，ECHILD/已退出拒绝旧 PGID | `process_group_fault_tests.cpp` 模拟 ECHILD 并拦截 uv_kill/kill 两种负 PGID 路径；`group-fault-red.log`；4292d94 |
| P0：HE 注册父 CancelHook 后 group frame 分配异常，没有 disarm，展开时 terminate | `net/connect.hpp:76` state 析构先 disarm；保留正常完成的显式 disarm | `connect_host_fault_tests.cpp` 仅让 group frame 分配失败，有 stop token；`he-fault-red.log`；a09e123 |
| P1：已选 winner 仍在 drain loser，Loop-first 关闭 winner 后返回成功的孤儿 TCP | `net/connect.hpp:195` drain 后检查 native closing，close 并返回 aborted | `connect_host_tests.cpp` 用忽略 token 但响应 ~Loop 的 loser 保持 drain，关闭 Loop 后检查 aborted；`he-loop-red.log`；765b8e4 |
| P0：DNS query factory 重入 close 后仍启动新 producer，破坏 native keepalive 关闭边界 | `dns/resolver.hpp:91,106` 用户 callback 后重检 stopping；不启动返回的 lazy task | `resolver_tests.cpp` factory close + producer body 未启动；`dns-reentry-red.log`；df5973c |
| P1：缓存插入用 clock 回调，重入 invalidate 后仍重发旧代次缓存 | `dns/resolver.hpp:180` clock 后重新检查 stopping/epoch | `resolver_tests.cpp` 第二次 clock 中 invalidate，最终缓存必须为空；同上；df5973c |

另增加确定性 maxAge overflow、LRU/entry/byte/subscriber 上限、默认不缓存、single-flight 单订阅取消、
Loop-first drain、backend 异常、首族失败继续、数字地址旁路、pacings、in-flight cancel/timeout、
成功不等待不可取消 DNS producer、真实祖孙进程组回收测试。GNU wrap fault fixtures 仅 Linux 注册。

## 实际 API / 保守偏差

- `DnsResolver(loop, DnsCacheOptions{}, query={}, clock={})`；`resolve(host,service,hints)`、`invalidate()`、
  `cacheSize()`、`close()`；返回 `Result<AddressList>`，immutable 地址列表可独立持有。
- 默认 cache off；maxAge 是本地年龄，不是 DNS TTL；无负缓存。最后订阅者立即 stop producer，无 grace。
  单订阅取消只返回 canceled；close/~Loop 的订阅返回 aborted；先完成者不被迟到取消覆盖。
- producer native async 平时 unref，有 flight 才 ref；先取消，再保证 drain，之后 close native。
  不强杀系统 NSS；注入后端必须最终完成/配合 teardown，关闭不承诺硬 deadline。
- `connect(loop,resolver,host,port,ConnectOptions{}) -> Task<Result<Tcp>>`；双族并行、50ms 偏好等待、
  250ms pacing/100ms 默认最小间隔（下限10ms）、默认并发4、每次5s/整体30s；
  第一成功取消落败者并 drain；业务失败继续，全败按最后发起候选选错，异常排空后重抛。
  sorting hook 只在族内，RFC8305 core subset，不虚报完整 RFC6724/NAT64。
  connector/clock/policy 遵循 loop-thread 契约，不支持逃离 uvco 生命周期的外部任务。
- `ProcessOptions::privateGroup` + `Process::signalGroup(int)`；不改变 reap-on-destruct，普通 spawn 无组能力。
  仅支持 loop-thread / libuv 唯一 reaper / WNOWAIT；leader 退出后不再持有可安全升级信号的 PGID 能力。
  组信号是 best effort，setsid/setpgid 逃逸不包含；Windows Job 明确延后。

## 验证

最终源码（含强化后的 bounds/kill 两路径回归）重新 configure/build：

| Linux 配置 | 完整 CTest | Block 2 + fs_event/fs_poll 定向重复 |
|---|---:|---:|
| g++ Debug | 59 targets × 3，全部通过 | 18 targets × 20，全部通过 |
| clang++ Debug | 59 × 3，全部通过 | 18 × 20，全部通过 |
| ASan + UBSan | 59 × 3，全部通过 | 18 × 20，全部通过 |
| TSan | 59 × 3，全部通过 | 18 × 20，全部通过 |

已逐个日志校验 target 次数（不是只看最终汇总）。包括 public headers、安装/搬迁、独立消费者、
add_subdirectory 与两 TU 检查。ASan detect_leaks=1、sanitizer halt_on_error=1，未发现报告；
process fixtures 沿用单独的原生 helper，库与测试主体保留 instrumentation。没有修改 sysctl/机器设置。
日志：`build/block2-cont/verified-{gcc,clang,asan,tsan}-{configure,build,tests,targeted}.log`。
已有 CI 自动纳入 CMake 新测试，无需修改 workflow；没有运行或声称远程 runner 已绿。

## 剩余事项

Windows/macOS/QNX 原生行为仍需目标环境验证；CI 的 macOS/Windows 仍为非阻塞实验项。
Windows Job、完整 RFC6724/NAT64、精确 TTY snapshot/独占租约、通用 fd poll 仍是明确后续里程碑。
系统 NSS/进程等待可能拖长 ~Loop；信号 disposition 与外部 reaper/mode owners 的任意混用不被承诺。
缓存预算是估算 owned 缓存，不是硬 allocator 上限，也不限制调用者保留的 shared 结果。
co2 detail 仍由 compat + CMake probe 约束，未来应推动公开扩展点。
