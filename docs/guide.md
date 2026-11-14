# uvco 使用手册

本手册按现行公开 API 编写，完整可编译程序在仓库 `examples/` 下，并作为测试目标随四套构建一起编译、运行。架构与不变量见 [architecture.md](architecture.md)，模块细则见 [modules/](modules/README.md)，时序见 [sequences.md](sequences.md)。

## 1. 环境要求

- C++14 编译器（已测试 g++ 13、clang++ 18）
- CMake ≥ 3.14（使用 `CMakePresets.json` 需 ≥ 3.21）
- libuv ≥ 1.40
- co2：`find_package(co2 CONFIG)` 或 FetchContent。预览包不接受数字版本号，因此查找时不带版本；但必须通过 `cmake/co2_capability.cpp`，其中包括左值 `co_await` 就地等待。这要求 co2 **`0be5ab2` 或更新**。FetchContent 锁定提交为 `0be5ab2f1b72be3755d8d13fb02d4bf32f652539`（`UVCO_CO2_GIT_TAG`）

## 2. 构建

在源码树根目录：

```sh
# 使用 presets（生成目录为 build/debug-gcc 或 build/debug-clang）
cmake --preset debug-gcc
cmake --build --preset debug-gcc
ctest --preset debug-gcc --output-on-failure --repeat until-fail:3

# 或手写构建目录
cmake -S . -B build/debug-gcc -DCMAKE_BUILD_TYPE=Debug -DUVCO_ENABLE_KCP=ON
cmake --build build/debug-gcc -j
ctest --test-dir build/debug-gcc --output-on-failure --repeat until-fail:3
```

常用选项：

| 选项 | 默认 | 含义 |
| --- | --- | --- |
| `UVCO_ENABLE_KCP` | OFF | 编译并导出 `uvco::kcp` |
| `UVCO_BUILD_TESTS` | 顶层工程 ON | 单元测试 |
| `UVCO_BUILD_EXAMPLES` | 跟随 `UVCO_BUILD_TESTS` | 本手册中的示例，并注册为 ctest |
| `UVCO_ENABLE_INSTALL` | 顶层 ON | 安装规则 |
| `UVCO_BUILD_KCP_BENCHMARK` | OFF | 需要同时打开 KCP |
| `UVCO_BUILD_PERF_BENCHMARK` | OFF | 与裸 libuv 对照的微基准 |

### 2.1 Sanitizer

本地验收与 CI 使用同一套组合。临时文件请放在 `build/` 下。

```sh
# ASan + UBSan（C 与 C++ 都要开，并禁止 recover）
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug   -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'   -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'   -DUVCO_ENABLE_KCP=ON
cmake --build build/asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1   ctest --test-dir build/asan --output-on-failure --repeat until-fail:3

# TSan（建议 clang++）
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++   -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -fno-sanitize-recover=all'   -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -fno-sanitize-recover=all'   -DUVCO_ENABLE_KCP=ON
cmake --build build/tsan -j
TSAN_OPTIONS=halt_on_error=1   ctest --test-dir build/tsan --output-on-failure --repeat until-fail:3
```

## 3. 安装与被其他项目使用

### 3.1 安装本库

依赖不捆绑进 uvco 的安装前缀。请先安装（或准备好）满足能力探测的 co2，以及 libuv ≥ 1.40，再安装 uvco。

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release   -DCMAKE_INSTALL_PREFIX=$PWD/build/stage -DUVCO_ENABLE_KCP=ON
cmake --build build/rel -j
cmake --install build/rel
```

### 3.2 find_package

```cmake
find_package(uvco CONFIG REQUIRED)
target_link_libraries(app PRIVATE uvco::uvco)
# 若安装时打开了 KCP：
# find_package(uvco CONFIG REQUIRED COMPONENTS kcp)
# target_link_libraries(app PRIVATE uvco::kcp)
```

消费者配置时会再次运行 co2 / libuv 探测。系统里若装着旧于 `0be5ab2` 的 co2，配置会失败并明确要求升级，而不是拖到编译期出现难懂的模板错误。

### 3.3 源码 FetchContent / add_subdirectory

```cmake
include(FetchContent)
# 也可以 add_subdirectory(path/to/co_uv)
FetchContent_Declare(uvco
    GIT_REPOSITORY https://github.com/lvyues1994/co_uv.git
    GIT_TAG        <你使用的提交>)
set(UVCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(uvco)
target_link_libraries(app PRIVATE uvco::uvco)
```

`uvco.hpp` 不包含 KCP；使用 KCP 时请额外 `#include <uvco/kcp.hpp>` 并链接 `uvco::kcp`。

## 4. 快速开始

每个在 loop 上跑的程序都遵循同一模式：构造 `Loop`（绑定当前线程）、写一个 `CO2_BEG` 协程、用 `uvco::run` 驱动到结束。完整程序见 `examples/01_quickstart.cpp`。

```cpp
#include <chrono>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

using namespace std::chrono_literals;

auto mainTask(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 1ms));
    if (not expect(r, "sleep")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, mainTask(loop));
}
```

错误以 `uvco::Result<T>` 返回：`if (r)` 表示成功，失败时用 `r.error()` / `r.message()`，不要在失败时调用 `r.value()`（那是契约违规）。用法错误（错线程、重复 wait、带着挂起操作析构句柄等）走契约处理器，不是 Result。

## 5. 分模块步骤

下列程序都可以直接编译运行（本机 loopback 或当前工作目录）。在仓库中打开 `UVCO_BUILD_EXAMPLES`（默认随测试打开）后，对应测试名为 `uvco-example-01-quickstart` 等。

### 5.1 TCP 回显（服务端与客户端在同一进程）

绑定 `127.0.0.1:0`，用 `localAddress()` 取得实际端口，先 `listen` 再 `connect`，内核 backlog 会保住这次连接。完整程序：`examples/02_tcp_echo.cpp`。

要点：

- 地址必须是数字 IP；主机名请用 [DNS / connect](modules/dns.md)。
- `buffer` 只是指针和长度。不要写 `buffer(std::string{"hello"})`：临时对象在挂起前销毁。
- 取消挂起的 `write` 会关闭整个流。`timeout(..., tcp.write(...))` 超时同样关流。详见 [sequences.md](sequences.md) 第 4 节。

```cpp
auto parsed = uvco::SocketAddress::parse("127.0.0.1", 0);
server.bind(*parsed);
server.listen();
auto addr = server.localAddress();
CO2_AWAIT_SET(r, client.connect(*addr));
CO2_AWAIT_SET(accepted, server.accept());
CO2_AWAIT_SET(r, client.write(uvco::buffer("hello", 5)));
CO2_AWAIT_SET(n, accepted->read(uvco::buffer(buf)));
```

只做监听、不想在同一类型上误用 `connect`/`write` 时，可以用 `TcpListener` / `PipeListener`。

### 5.2 UDP

完整程序：`examples/03_udp.cpp`。`recv` 是拉模式，没有等待者时数据报留在内核缓冲。`send` 一经发出不可取消。`Datagram::truncated` 表示用户缓冲比数据报短。

### 5.3 文件

完整程序：`examples/04_fs.cpp`。`writeFile` / `readFile` 适合整文件；零额外分配的单次操作请把 `fs::StatOp`、`OpenOp` 等放进 `CO2_BEG` 局部列表再以左值等待。`File` 析构会同步关闭仍打开的 fd。

```cpp
CO2_AWAIT_SET(w, uvco::fs::writeFile(loop, path, std::string{"uvco-fs"}));
CO2_AWAIT_SET(text, uvco::fs::readFile(loop, path));
CO2_AWAIT_SET(u, uvco::fs::unlink(loop, path));
```

### 5.4 文件监视

完整程序：`examples/05_watcher.cpp`。示例用 `FsPoll`（定时 stat，可移植性较好）。Linux 上也可用 `FsEvent::watch`；QNX 等没有 fsevents 的后端会返回 `ENOSYS`。`next()` 拉下一条变更；`stop`/`close` 让挂起的 `next` 变为 `aborted`。

信号与相位 hook 见 [modules/signal-hooks-tty.md](modules/signal-hooks-tty.md)。`Signal` 拒绝监视 `SIGPIPE`。`onPrepare` / `onCheck` 默认 unref；`onIdle` 创建后是停止的，启动后即使 unref 也可能空转。

### 5.5 子进程与捕获

完整程序：`examples/06_process_capture.cpp`。示例使用仓库编译的 helper，不依赖系统里的 `echo`。`ProcessOptions::args` 不含 argv[0]。

务必记住回收契约：

- 取消 `wait` **不**杀子进程。
- 析构把仍在运行的孩子交给后台回收，不发信号。
- 只有 `detach()` 把回收责任交给你。
- `~Loop` 会等待未 detach 的孩子，可能一直阻塞；需要时先 `kill`。

`communicate` 默认 `CaptureCancel::stopWaiting`：取消时关掉本次管道并停止等待，孩子仍由 reaper 回收。输出超过上限返回 `ENOBUFS`，不交付部分输出。

### 5.6 timeout 与 whenAny

完整程序：`examples/07_timeout_whenany.cpp`。`timeout` 先启动主操作；主操作先完成则计时器不启动。父取消得到 `canceled`，不是 `timed_out`。`whenAllResults` 要等全部成功，void 元素会从 tuple 里省略。忽略取消且不结束的分支会挡住整个组合子。

### 5.7 取消

完整程序：`examples/08_cancellation.cpp`。把 `stop_token` 传给 `run` 作为根令牌，或在已有 `Task` 树上 `request_stop`。已请求停止时，许多操作即使可以立即完成也返回 `canceled`（先停止规则）。`close()` 中止操作为 `aborted`，不要与取消混淆。

### 5.8 queueWork

完整程序：`examples/09_work.cpp`。闭包在 libuv 全局线程池运行（与 fs、DNS 共享），完成后回到 loop。签名可以是 `R()` 或 `R(co2::stop_token)`。排队中取消可能让 fn 不运行；一旦 fn 开始执行就不会被强制打断。**已经算完的结果不会被晚到的取消丢掉**，副作用也不会回滚。

### 5.9 KCP（可选）

需要 `-DUVCO_ENABLE_KCP=ON`，`#include <uvco/kcp.hpp>`，链接 `uvco::kcp`。完整程序：`examples/10_kcp.cpp`（测试名 `uvco-example-10-kcp`）。

```cpp
auto left = uvco::KcpEndpoint::create(loop, *bind);
auto right = uvco::KcpEndpoint::create(loop, *bind);
auto a = left->openSession(*right->localAddress(), /*conv=*/1);
auto b = right->openSession(*left->localAddress(), 1);
CO2_AWAIT_SET(w, a->sendMessage({'p','i','n','g'}));
CO2_AWAIT_SET(m, b->recvMessage());
```

共享端口上接受未知 conv：创建 endpoint 时打开 `KcpEndpointOptions::accept`，`CO2_AWAIT_SET(incoming, endpoint.acceptPeer())`，再 `openSession(std::move(*incoming))` 或 `openStream(...)`。字节流 API 是 `KcpStream::{readSome, write, drain}`。`examples/10_kcp.cpp` 在同一程序里演示了会话、`acceptPeer` 和 stream。

KCP 核心不碰系统调用；丢包后即使没有新的收包事件，也会靠 `ikcp_check` 预约的定时器重传。loopback 无丢包时它不会比 TCP 更快。默认收到包会立刻 flush ACK。

## 6. 错误处理模式

- **检查 Result**：`if (not r) return r.error();` 或记录 `r.message()`。
- **不要在失败时 `*r` / `r.value()`**，不要在成功时 `r.error()`。
- **工厂**：`Tcp::create`、`Signal::create`、`KcpEndpoint::create`、`spawn` 在资源不足或 init 失败时返回 Result。
- **异常**：`queueWork` 的 callable、`whenAny` 分支里抛出的 C++ 异常会在 loop 线程、于兄弟分支结束后重抛。这不是业务错误码的替代品。
- **契约违规**：错线程、队列溢出、并发第二个 `read`/`wait`/`next`、带着挂起操作析构句柄、带着 `KeepAlive` 析构 Loop。可通过 `co2::setContractViolationHandler` 安装处理器（进程级，与 co2 共用）。

## 7. 实践建议与陷阱

1. **所有 libuv 与 sync 原语只在 loop 线程调用。** 从线程池回来先 `CO2_AWAIT(co2::scheduleOn(loop))`。跨线程投递后还要让 loop 继续跑，请持有 `Loop::KeepAlive`。
2. **缓冲区寿命必须覆盖整个 await。** 无栈协程会在挂起前销毁完整表达式里的临时量。
3. **跨挂起点的局部变量必须写在 `CO2_BEG` 列表里。** 在函数体里用 `auto x = ...;` 再 `CO2_AWAIT` 会触发“jump to case label / crosses initialization”。不能默认构造的类型（例如 `Result<KcpSession>`）不要放进该列表，改为在 `main` 里创建再传入引用，或用 `unique_ptr`。
4. **写取消会关流。** 需要限时写时，想清楚超时后这个连接是否还能用。
5. **子进程回收。** 不要假设析构会发 SIGTERM；也不要在有活着的孩子时随意销毁 Loop。
6. **大请求用帧局部左值。** `Sleep`、`fs::StatOp`、`getaddrinfo` 的右值可能超过 64 字节槽而分配。`WriteAwaiter` 热路径已经是 0 分配。
7. **Channel / Mutex / Event 不是线程安全的。**
8. **KCP 的 conv 不是认证。** 连接管理、心跳和关闭要自己做。`output` 的缓冲区必须先拷走再异步发送（运行时已经这么做）。
9. **不要在 `run()` 里再 `run()` 同一 Loop。** 同一 loop 可以顺序多次 `run`，也可以在 `run` 返回后 `drain`。
10. **hook 默认 unref。** 不要指望只靠一个 unref 的 `onPrepare` 让 `uv_run` 不退出；活动的 `Idle` 即使 unref 也可能把 CPU 打满。

## 8. 常见问题

**Q：和直接用 libuv 比会慢多少？**  
A：本机 loopback 上封装开销大约 1.5%–5%，见 [perf-review.md](perf-review.md)。这不是所有负载的固定损耗。热路径 write 在优化后不再为 awaiter 分配。

**Q：为什么系统里的旧 co2 配不过？**  
A：从 `4240777` 起，左值 ReqAwaiter 不再自己做指针包装，需要 co2 就地等待左值。请升级到 `0be5ab2` 或让 uvco FetchContent 去拉锁定提交。

**Q：`whenAny` 的其他成功值去哪了？**  
A：目前仍保留在各分支里直到全部结束后丢弃（或随 Task 销毁）。“只取出胜者”的专用提取 API 尚未批准。

**Q：macOS / Windows / QNX 能用吗？**  
A：代码按可移植性编写，CI 上 macOS/Windows 是非阻塞实验任务。QNX 的能力与限制见 [qnx.md](qnx.md)。请在目标环境自行验证。

**Q：如何写一个多客户端的 echo 服务？**  
A：`listen` 之后循环 `accept`，对每个连接 `co2::spawn(loop, session(std::move(*client)))`，主协程继续 accept。结束时 `drain(loop)`，并确保没有被遗忘的 `KeepAlive`。会话里用 `read`/`write`，读到 `eof` 后 `shutdown` 或 `close`。
