# 独立 review 修复说明

以下为独立 review 的 P1/P2 项逐项处理；公开语义变化均在 Unreleased changelog 中列出。

| 项目 | 处理 | 稳定回归位置 |
|---|---|---|
| P1-1 pending write 不响应 stop，timeout 无法 drain | 每个 pending write 注册 CancelHook；stop 关闭整个流，等 callback 后 canceled；显式 close 仍 aborted | tests/write_cancel_tests.cpp：timeout + 直接 stop |
| P1-2 双族 DNS 全败固定取 IPv6，掩盖 IPv4 暂时故障 | 优先 EAI_AGAIN，再非永久 miss，最后稳定按 IPv6；连接候选全败规则不变 | tests/connect_host_tests.cpp：两种族顺序 |
| P1-3 缺少许可证/变更记录 | MIT、2026 yu.shang（来自 git user.name）；Unreleased 0.1.0 changelog；安装包也带 MIT notice | tests/package_tests.cmake.in |
| P2-4 构造错误风格不明确 | Tcp/Udp/Timer/DnsResolver 增加 create Result 工厂；内部 OOM→no_memory，native Errc 保真；旧 convenience 规则明确保留 | tests/factory_fault_tests.cpp：四类 native/OOM/success |
| P2-5 Work 晚到取消丢弃结果 | 只有成功 dequeue 跳过 callable；运行中协作取消，函数完成仍交付结果/void，异常 drain 后重抛 | tests/work_tests.cpp：queued/running/late/pre-cancel |
| P2-6 部分写入失败无进度信息 | 前缀发送后提交/分配失败关闭流；queued callback 失败也关闭。Result<void> 不提供偏移，不允许原流整包重试 | tests/write_partial_fault_tests.cpp |
| P2-7 noexcept schedule 内部分配 | 四队列构造期 reserve；schedule/drain 零 C++ 分配，保留原 batch/resume 顺序 | tests/schedule_fault_tests.cpp：禁止 new 的 local/remote 投递与 drain |
| P2-8 过密格式 | .clang-format + 四指定 header 单独 style commit；保留 CO2 宏，非注释 C++ token stream 完全相同 | 格式前后 token 检查、公共头和全套编译 |
| P2-9 ExitStatus 命名 | 0.1 前改为 exitStatus / termSignal，不保留别名；所有仓库调用者迁移 | process/capture/group/portable/Signal tests |
| P2-10 README 临时验证记录 | README 保留用户构建/使用/边界；历史记录移至 verification.md，不含本机临时路径 | 文档检查 |

## 重要取舍

- reserve 不是声称无限容量：Loop 默认每队列65536项，64位四队列约2MiB保留容量，可构造时调整。
  超出显式配置上限为契约违规；不在 OOM 时静默丢任务、重入 resume 或临时分配。
  这保持当前 co2 Scheduler 的 void/noexcept 协议；未来公开 intrusive node 可去掉这个容量边界。
- legacy handle convenience 构造器保持历史 fail-fast；DnsResolver throwing ctor 现在保留 uv_category 的 system_error，
  不把 EMFILE 等误报为 bad_alloc。资源失败可恢复的代码应选 create/watch/open/spawn 等 Result 工厂。
- write 取消会影响同流其他操作；这不是事务回滚，已经发送的字节不能撤销。
- Work 不强杀线程，也不假造已完成副作用被撤销的 canceled 结果。

最终验证与未覆盖平台见 [verification.md](verification.md)。没有 push，也没有声称远程 CI 已运行。
