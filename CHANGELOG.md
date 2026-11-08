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
- pending write 取消默认关闭整个流，等待原生回调后返回 canceled。
- 部分写入后失败关闭流，禁止把未知偏移当作可安全重试。
- Work callable 已完成时保留结果，晚到取消不撤销副作用。
- 双族 DNS 失败保留可重试错误；ExitStatus 字段采用 exitStatus/termSignal。
- noexcept scheduler 使用构造期预分配的显式有界队列。

### Limitations
- KCP 仍在验收：上游 IOFFSETOF 空指针成员取址触发 UBSan，尚未豁免；benchmark/KCP CI 与部分寿命/包回归未完成。
- HE 是 RFC8305 核心子集；process group 不是严格进程树 containment。
- Windows Job、精确 TTY snapshot guard、完整 RFC6724/NAT64 尚未实现。
- macOS/Windows/QNX 原生支持仍需目标环境验证。
