# uvco 文档索引

从这里进入当前版本的设计与用法。文档按现行代码编写，专项审查里过时的细节以架构文档、模块文档与本索引为准。

## 入门

| 文档 | 内容 |
| --- | --- |
| [../README.md](../README.md) | 项目简介、构建预设、安装与模块概览 |
| [guide.md](guide.md) | 完整使用手册：环境、预设、sanitizer、安装/FetchContent、分模块步骤与 FAQ |
| [../examples/](../examples/) | 手册中的可编译示例，作为 `uvco-example-*` 测试随构建运行 |

## 设计

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | 当前架构：目标/非目标、分层、核心概念、线程与关闭模型、错误/取消/分配、构建选项 |
| [modules/README.md](modules/README.md) | 模块设计：职责、公开 API、完成/取消/关闭语义、内部要点与限制 |
| [sequences.md](sequences.md) | 关键路径时序图（ReqAwaiter、取消、关流、Loop 关闭、Process、HE、watcher、Work、KCP） |
| [architecture-history.md](architecture-history.md) | 刷新前的架构叙述与实现演进，供对照，不作为现行契约 |

## 已批准的专项方案

这些文档记录当时批准的设计。实现若有后续修订，以 [architecture.md](architecture.md) 与 [modules/](modules/README.md) 为准。

| 文档 | 内容 |
| --- | --- |
| [design-watchers.md](design-watchers.md) | 统一 watcher 内核（Backend、EventPort / HookSink、DeliveryPolicy） |
| [design-kcp.md](design-kcp.md) | 可选 KCP 适配层：纯算法核心、外部时钟与 I/O、消息/stream、acceptPeer |

## 性能、验证与审查记录

| 文档 | 内容 |
| --- | --- |
| [perf-review.md](perf-review.md) | 数据路径拷贝与分配审计；§1–5 为初次基线，§6 为已落地的三项优化与复测 |
| [benchmark-kcp.md](benchmark-kcp.md) | KCP 本机 loopback / 模拟弱网基准（不是同条件 TCP 对照证明） |
| [verification.md](verification.md) | 验证范围、构建组合与已知平台限制 |
| [qnx.md](qnx.md) | libuv / uvco 在 QNX 上的能力与限制（`fs_event` 为 ENOSYS） |
| [block2-review.md](block2-review.md) | Block 2 watcher 审查记录 |
| [review2.md](review2.md) | 第二轮审查与已批准的修复决策 |

## 阅读顺序建议

1. 想动手：先 [guide.md](guide.md)，对照 `examples/`。
2. 想理解不变量：再读 [architecture.md](architecture.md) 和 [sequences.md](sequences.md)。
3. 查某个 API 的取消或关闭规则：打开对应的 [modules/](modules/README.md)。
4. 做性能或移植决策：看 [perf-review.md](perf-review.md)、[verification.md](verification.md)、[qnx.md](qnx.md)。
