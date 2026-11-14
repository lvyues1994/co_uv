# 模块：Stream / Tcp / Pipe / BufferedReader

## 职责

在 libuv 流（TCP、管道、以及 TTY 的读写部分）上提供拉模式读写、监听/接受与关闭写端。缓冲区描述符不拥有内存。`BufferedReader` 是可选的拥有型字符串适配；`TcpListener` / `PipeListener` 是只含 bind/listen/accept 的薄门面。

头文件：`<uvco/stream.hpp>`。TTY 的模式与窗口见 [signal-hooks-tty.md](signal-hooks-tty.md)。

## 公开 API

### 缓冲区与地址

| 名称 | 说明 |
| --- | --- |
| `MutableBuffer` / `ConstBuffer` | `(指针, 长度)`，不拥有内存。数据必须活到对应 read/write 恢复。 |
| `buffer(ptr, n)` / `buffer(string&)` / `buffer(vector<char>&)` / `buffer(char(&)[N])` | 辅助构造。`buffer(std::string&&)` 已删除：无栈协程里临时对象在挂起前销毁，会悬空。 |
| `SocketAddress::parse` / `ipv4` / `ipv6` | 只解析数字 IP，不做 DNS。`port()` / `ip()` / `toString()`。 |

### 流上共有（Tcp / Pipe，TTY 有读写子集）

| 名称 | 说明 |
| --- | --- |
| `read(buf)` | `Result<size_t>`。拉模式：仅在有读者时 `uv_read_start`，读到一块就 `uv_read_stop`。libuv 直接读进用户缓冲。对端关闭写端后返回 `eof`。同时只能有一个读者。可取消。 |
| `write(buf)` / `write(bufs)` | `Result<void>`。先 `uv_try_write`，剩余部分 `uv_write` 排队。可并发，按调用顺序。出错时不知道写出了多少字节。 |
| `shutdown()` | `Result<void>`。等排队的写完成后关闭写端。**不可取消**。 |
| `listen(backlog=128)` / `accept()` | accept 拉模式：没有 accept 挂起时 libuv 可保留一个待接受连接并暂停监听 fd。同时只能有一个 accept。可取消。`accept` 返回 `Result<Tcp>` 或 `Result<Pipe>`。 |
| `isReadable()` / `isWritable()` / `writeQueueSize()` | 查询。 |
| `close()` | 见 core 句柄规则。 |

### Tcp

| 名称 | 说明 |
| --- | --- |
| `Tcp(loop)` / `Tcp::create(loop)` | 只可移动。地址必须是数字 IP。 |
| `Tcp::open(loop, uv_os_sock_t)` | 成功才移交 fd；失败由调用者关闭。类型必须正确且未被其他句柄拥有。 |
| `bind(address, flags=0)` | 绑定。 |
| `connect(address)` | `Result<void>`。取消会**关闭整个 Tcp**，之后操作返回 `bad_descriptor`。 |
| `setNoDelay` / `setKeepAlive` | TCP 选项。 |
| `localAddress()` / `peerAddress()` | `Result<SocketAddress>`。 |

### Pipe

Unix 域套接字 / Windows 命名管道。**不支持 IPC（传递句柄）**，也不支持 Linux 抽象套接字（需要更高版本 libuv）。

| 名称 | 说明 |
| --- | --- |
| `Pipe(loop)` | 构造即初始化。无 `create` 工厂（构造失败走契约检查）。 |
| `Pipe::open(loop, uv_file)` | 与 Tcp::open 相同的所有权规则。 |
| `bind(path)` / `connect(path)` | 取消 connect 同样关闭整个 Pipe。 |
| `localName()` / `peerName()` | `Result<string>`。 |

### Listener 与 BufferedReader

- `TcpListener` / `PipeListener`：只暴露 bind/listen/accept/close 与本地地址，accept 仍返回 Tcp/Pipe。
- `BufferedReader<Stream>(stream)`：`readExactly(n)` / `readLine(maxLength)` / `readToEnd(maxBytes)` → `Result<string>`。readLine 不含 LF、保留 CR；EOF 先交付末尾未换行行，再返回 `eof`。cap/EOF/取消保留未消费缓冲，可用更大 cap 重试。reader 与 stream 必须活过操作；不得并发，也不得绕过 reader 直接读底层流。

## 语义

- **先停止**：停止已请求时立即 `canceled`，即使本可以立即完成。
- **关闭/析构 Loop**：挂起的 read/accept/write/shutdown/connect 以 `aborted` 结束。
- **写取消**：挂起的 write 被 stop_token 取消时，**关闭整个流**，等待原生回调后返回 `canceled`。因此 `timeout(..., tcp.write(...))` 超时也会关流。部分写入后失败同样关流，禁止把未知偏移当作可安全重试。
- **已关闭的流**（包括取消 connect 之后）：操作返回 `bad_descriptor`。

## 内部要点

- 零用户态 payload 拷贝：缓冲区直接交给 libuv。内核拷贝无法避免。
- `WriteAwaiter` ≤ 64 字节；`try_write` 成功的右值等待 0 次堆分配。未写完才为原生 Req 分配，并把 `CancelHook` 放在 pending Req 上。
- BufferedReader 把数据直接读进 string 尾部，异常/取消回滚。

## 限制

- 不能在不关闭流的情况下取消写。
- Pipe 无 IPC、无抽象套接字。Windows 命名管道与 `uv_try_write` 语义未在本仓库实机验证。
- `open()` 失败不承诺恢复 nonblocking 等 flags；libuv 不一定拒绝错误类型的描述符。
- BufferedReader 的 cap 是输出限制，不是全局分配器限额。
