# uvco 模块设计

各模块的职责、公开 API、完成/取消/关闭语义、内部要点与限制。整体分层与不变量见 [../architecture.md](../architecture.md)，时序见 [../sequences.md](../sequences.md)，用法见 [../guide.md](../guide.md)。

| 文档 | 覆盖范围 | 头文件 |
| --- | --- | --- |
| [core.md](core.md) | Loop、Result、句柄、取消、yieldNextTurn | `<uvco/core.hpp>` |
| [timer.md](timer.md) | Timer、Sleep | `<uvco/timer.hpp>` |
| [sync.md](sync.md) | Event、Mutex、Channel | `<uvco/sync.hpp>` |
| [combinators.md](combinators.md) | whenAny、whenAllResults、timeout、runUntil | `<uvco/sync.hpp>` |
| [stream.md](stream.md) | Tcp、Pipe、Stream、BufferedReader、Listener | `<uvco/stream.hpp>` |
| [udp.md](udp.md) | Udp | `<uvco/udp.hpp>` |
| [dns.md](dns.md) | getaddrinfo、DnsResolver、Happy Eyeballs `connect` | `<uvco/dns.hpp>`、`<uvco/net/connect.hpp>` |
| [fs.md](fs.md) | File、路径操作、Dir、FsEvent、FsPoll | `<uvco/fs.hpp>` |
| [process.md](process.md) | Process、capture / communicate | `<uvco/process.hpp>` |
| [signal-hooks-tty.md](signal-hooks-tty.md) | Signal、Idle/Prepare/Check、TTY | `<uvco/watcher.hpp>`、`<uvco/stream.hpp>` |
| [work.md](work.md) | queueWork | `<uvco/work.hpp>` |
| [kcp.md](kcp.md) | KcpCore / Endpoint / Session / Stream（可选） | `<uvco/kcp.hpp>`，链接 `uvco::kcp` |
