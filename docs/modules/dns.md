# 模块：DNS 与 Happy Eyeballs

## 职责

- 一次性 `getaddrinfo` / `getnameinfo`。
- 可选的 `DnsResolver`：默认关闭的 LRU 缓存与 single-flight。
- `connect(loop, resolver, host, port, options)`：RFC 8305 **核心子集**的双栈连接，不是完整 RFC 6724 / NAT64。

头文件：`<uvco/dns.hpp>`、`<uvco/net/connect.hpp>`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `getaddrinfo(loop, host, service = {}, hints = {})` | `Result<vector<SocketAddress>>`，系统顺序。字符串按值持有；hints 只复制四个输入标量。 |
| `getnameinfo(loop, address, flags = 0)` | `Result<NameInfo>`（host / service）。 |
| `DnsCacheOptions` | `enabled=false`（默认关闭）、容量、TTL、是否缓存失败等以头文件为准；无负缓存，TTL 非权威。 |
| `DnsResolver(loop, options = {}, query = {}, clock = {})` / `create(...)` | `query` 可注入解析函数（测试用）。 |
| `resolver.resolve(host, service = {}, hints = {})` | 与 getaddrinfo 同类结果；缓存命中时不发请求。 |
| `invalidate()` / `cacheSize()` / `close()` | 清缓存、查询、关闭。 |
| `ConnectOptions` | 含可选 `addressPolicy`：在各族系统顺序内排序，协调器只做族间交错，不是完整 RFC 6724。 |
| `connect(loop, resolver, host, port, options = {})` | `Task<Result<Tcp>>`。 |

## 语义

- **取消**：getaddrinfo/getnameinfo 以回调入口检查 stop_token 为线性化点——此时已取消则释放并丢弃结果；之后的取消不推翻完成。
- **缓存**：关闭时行为等于每次查询。开启后相同查询 single-flight，最后一个订阅者离开即取消，没有 cache-fill grace。
- **关闭**：Resolver 用 resident / native async 保活，而不是外部 KeepAlive。`close` / `~Loop` 必须等待 producer 最终完成。
- **HE**：交错 IPv6/IPv4，有限并发与节奏；固定最多 64 个 worker。双族失败会保留可重试错误。成功返回已连接的 `Tcp`。

## 内部要点

native `addrinfo` 在恢复协程前释放；投影 `bad_alloc` 返回 `ENOMEM`，其他异常清理后重抛。共享 DNS 结果会复制到连接器拥有的候选数组，以保证独立排序和生命周期。

`connectOne` 对单个数字地址 `Tcp::connect`，失败则 close 该套接字。

## 限制

- 默认不做缓存；开启后仍非权威 TTL，无负缓存。
- 不可取消的 NSS / 注入后端可能阻塞 Resolver 与 Loop 析构，不承诺硬 deadline。
- 不是完整 RFC 6724 / NAT64。族内 policy hook 只影响族内顺序。
- getaddrinfo 与 fs、Work 共享全局线程池。
