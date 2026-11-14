# 模块：KCP（可选）

## 职责

把 skywind3000/kcp 的纯算法核接到 uvco 的 UDP 与 Timer 驱动上，提供消息会话、字节流会话，以及多 conv 共享一个 UDP 端口时的显式 `acceptPeer`。默认不编译，需 `UVCO_ENABLE_KCP=ON` 并链接 `uvco::kcp`。

头文件：`<uvco/kcp.hpp>`。原则与契约见 [design-kcp.md](../design-kcp.md)。

三条不可违背的边界：

1. `ikcp.c` 原样嵌入，不打补丁。`KcpCore` 不拿 socket、不读时钟、不碰 Loop；时间从外部传入。vendor 部分 OOM 路径不可恢复，已在文档写明。
2. 系统调用全在外层。`output` 回调只把借用的字节拷进有界队列，不发起 I/O，不挂起，不重入核心。
3. 用 `ikcp_check` 精确预约定时器；默认收到包后马上 flush（可用基准关闭对照）。默认参数接近 `nodelay=1, interval=20, resend=2`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `CoreOptions` | 默认 MTU 1200、消息上限 64KiB、发送窗 128、接收窗 128、interval 20ms、fastResend 2、拥塞控制开。`turbo()` 把 interval 调到 10 并关闭拥塞控制。 |
| `KcpCore::create(conv, options, sink)` | 纯同步核，供确定性测试或自管 I/O。 |
| `KcpEndpoint::create(loop, bind, options = {})` | 共享 UDP 端点。`KcpEndpointOptions` 含 mtu、packetLimit、sessionLimit、是否 accept、候选限制与 TTL。 |
| `openSession(peer, conv, options)` / `openStream(...)` | 主动打开。stream API 会把 `core.stream=true`。同一 peer+conv 不能重复打开（`EADDRINUSE`）。 |
| `acceptPeer()` | `Result<KcpIncoming>`。仅当 `options.accept==true`。主动拉取未知 conv 的首包。 |
| `openSession(incoming, options)` / `openStream(incoming, …)` | 把 candidate 升级为会话，并把首包喂进核心。incoming 只能用一次，且须属于该 endpoint、未过期。 |
| `KcpSession::{sendMessage, recvMessage, drain, stats, abort}` | 消息模式。`KcpMessage` 即 `vector<char>`。`sendMessage` 按值：左值先拷贝。 |
| `KcpStream::{readSome, write, drain, stats, abort}` | 字节流。`write` 返回 `Result<size_t>`。 |
| `KcpSessionOptions` | 含 `core`、`immediateFlushAfterInput`（默认 true）、消息条数/字节高低水位与硬限制、输出队列上限、持续拥塞保护参数。 |

## 语义

- **完成**：`sendMessage` 在通过背压并交给核心后完成（不是对端 ACK）。`drain` 等到输出队列与核心缓冲清空或会话失败。`recvMessage` 交付一条完整应用消息。
- **取消**：收发等待可取消，为 `canceled`。取消不自动等价于协议 FIN（KCP 没有 FIN）。
- **关闭**：`close()` / `abort` / `~Loop` 中止挂起操作为 `aborted` 或指定错误。endpoint 关闭会停掉收发泵并关闭底层 UDP。
- **背压**：超过硬限制或持续拥塞保护触发时发送失败；高低水位控制何时挂起/唤醒发送者。
- **时钟**：由 runtime 用 `uv_hrtime` 转毫秒后传入 `ikcp_update`/`ikcp_check`，再预约下一次 Timer。丢包后即使没有收包事件也会重传。

## 内部要点

C 适配翻译单元在包含 vendor `ikcp.c` 前把 `IOFFSETOF` 重定义为标准 `offsetof`，vendor 字节与 SHA256 不变。多个 conv 共用 endpoint 的 receive/transmit 泵（`CO2_BEG_MEMBER`）。TX 当前串行。

## 限制

- 默认 OFF。无握手、认证、加密、FIN。conv 不是身份证明。
- vendor 部分 OOM 不可恢复。接收窗等参数有校验范围（例如接收窗 ≥ 128 且 ≤ 65535）。
- 本机 loopback 无丢包时 KCP 不会比 TCP 更快（多一层 UDP+算法）。弱网相对 TCP 的 30%–40% 主张尚未在同条件 `tc netem` 对照中验证。
- 跨平台实机验证不足。
