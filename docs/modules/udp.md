# 模块：Udp

## 职责

提供拉模式的 UDP 套接字：每次 `recv` 取一个数据报，直接读进调用者缓冲区；`send` 先尝试 `uv_udp_try_send`。地址只接受数字 IP。

头文件：`<uvco/udp.hpp>`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `Udp(loop)` / `Udp::create(loop)` | 只可移动。 |
| `bind(address, flags=0)` | 绑定。 |
| `connect(peer)` / `disconnect()` | 可选的默认对端；`send(buf)` 无地址重载走已 connect 的套接字。 |
| `send(to, buf)` / `send(buf)` | `Result<void>`。发起后**不可取消**。 |
| `recv(buf)` | `Result<Datagram>`。`Datagram::{size, sender, truncated}`。缓冲区偏小则丢弃多余字节并置 `truncated`。 |
| `setBroadcast` / `setTtl` | 套接字选项。 |
| `setMembership` / `setSourceMembership` | 组播加入/离开。 |
| `setMulticastLoop` / `setMulticastTtl` / `setMulticastInterface` | 组播选项，错误透传 libuv。 |
| `sendQueueSize()` / `sendQueueCount()` | 发送队列统计。 |
| `localAddress()` / `peerAddress()` | `Result<SocketAddress>`。 |

## 语义

- **拉模式**：只有 `recv()` 挂起时才接收。没有等待者时到达的数据报不在用户态排队，留在内核套接字缓冲（满了由内核丢弃）。
- **取消 recv** → `canceled`。`close()` / `~Loop` → 挂起的 recv/send 以 `aborted` 结束。
- **send 不可取消**。同一套接字上 recv 同时只能有一个（契约）。

## 内部要点

- recv 的完成总是推迟到下一轮（经 `loop.schedule`）恢复，并且在 recv 回调之外才 `uv_udp_recv_stop`。原因是 Linux 上 libuv 在 EPOLLERR 之后读错误队列时断言套接字仍在接收。
- 64 字节的 send awaiter 可放入默认槽；`try_send` 快路径无额外装箱。fallback 分配一次原生 send 请求。

## 限制

- 不做 DNS；不做用户态收包缓冲或重排。
- 组播在不支持的平台上返回 libuv 的错误，uvco 不另做兼容层。
