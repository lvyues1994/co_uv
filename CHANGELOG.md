# Changelog

## [Unreleased] — 0.1.0

尚未发布。公开 API 在 0.1.0 发布前仍可能调整。

### Added
- 实验 KCP 可选 target（默认 OFF）：固定原样 vendor、纯核、消息/stream、共享 UDP endpoint 与有界显式 accept。
- C++14/co2 + libuv 的 Result、Loop、请求/句柄生命周期与取消基础层。
- Timer/sleep、Event/Mutex/Channel、whenAny/whenAllResults/timeout、yieldNextTurn。
- Tcp/Pipe/TTY、零缓冲流、BufferedReader、UDP 与 listener 门面。
- File、目录迭代、FsEvent/FsPoll、Signal、Idle/Prepare/Check hooks。
- Process spawn/wait、capture/communicate、POSIX private-group signaling。
- DNS 解析、可选有界缓存/single-flight、Happy Eyeballs 核心子集、queueWork。
- CMake install/export、消费者检查、Linux CI 与实验 macOS/Windows jobs。
- MIT LICENSE；Result create 工厂与统一格式规则。

### Changed
- BufferedReader 与 KcpEndpointBlock 的泵函数改为 CO2_BEG_MEMBER 成员协程，去掉静态 *Task/self 转接；公开行为不变。
- ReqAwaiter 不再提供 `operator_co_await() &` 包装：左值等待改由 co2 就地完成；`find_package(co2)` 需通过就地左值能力探测（0be5ab2 或更新）。公开等待语义不变。
- FetchContent 锁定的 co2 升级到 `0be5ab2f1b72be3755d8d13fb02d4bf32f652539`（仍为 0.3.0-pre）；公开 API 与热路径 64B 槽不变。
- WriteAwaiter 热路径保持 co2 默认64B槽内联，pending native Req 才拥有 CancelHook；try_write 成功的右值 await 不再装箱分配。
- BufferedReader 与 process capture 直接读入带异常/取消回滚的 string 尾部；已提交字节、输出 cap、EOF 与 reap 契约不变。
- Work 构造不创建取消状态：无 token callable 不分配内部停止状态，合作式 callable 每次真正提交创建新状态；晚到取消规则不变。
- KCP采用自有C offsetof适配TU，保留vendor哈希，所有sanitizer继续开启。
- KCP合并每batch显式flush，新增字节高低/硬水位、持续输出拥塞保护和超限消息终止。
- KcpStream leftover不再误用发送消息上限，write遵守终止错误优先级；拒绝16位wire接收窗溢出。
- KCP native close经schedule恢复，其他句柄默认策略不变。
- Linux CI启用KCP，增加KCP包/两TU与pending-lifetime回归、可选loopback/模拟时延基准。
- pending write 取消默认关闭整个流，等待原生回调后返回 canceled。
- 部分写入后失败关闭流，禁止把未知偏移当作可安全重试。
- Work callable 已完成时保留结果，晚到取消不撤销副作用。
- 双族 DNS 失败保留可重试错误；ExitStatus 字段采用 exitStatus/termSignal。
- noexcept scheduler 使用构造期预分配的显式有界队列。

### Limitations
- KCP 原样 vendor 保留部分OOM不可恢复限制；TX串行，缺少握手/认证/FIN，不宣称公网时延优势或未验证平台支持。
- HE 是 RFC8305 核心子集；process group 不是严格进程树 containment。
- Windows Job、精确 TTY snapshot guard、完整 RFC6724/NAT64 尚未实现。
- macOS/Windows/QNX 原生支持仍需目标环境验证。
