# uvco

[![CI](https://github.com/lvyues1994/co_uv/actions/workflows/ci.yml/badge.svg)](https://github.com/lvyues1994/co_uv/actions/workflows/ci.yml)


基于 [co2](https://github.com/lvyues1994/coro)（C++14 无栈协程库）对 libuv 的协程封装。
设计见 [docs/architecture.md](docs/architecture.md)。

提供 Loop/Result、timer、sync/组合器、Tcp/Pipe/TTY、UDP、文件/目录与 fs watchers、
Signal/phase hooks、Process/capture、DNS resolver/cache、Happy Eyeballs 核心子集及线程池 Work。
库为 header-only，错误边界、取消与关闭规则见下文及架构文档。

## 要求

- C++14 编译器（已测试 g++ 13、clang++ 18）
- CMake ≥ 3.14（使用 CMakePresets.json 需 ≥ 3.21）
- libuv ≥ 1.40：优先 `find_package(libuv)`，其次 pkg-config，最后 FetchContent（v1.48.0）
- co2：优先 `find_package(co2 CONFIG)`，否则 FetchContent（固定提交，见 `UVCO_CO2_GIT_TAG`）

## 构建与测试

```sh
cmake --preset debug-gcc   && cmake --build --preset debug-gcc   && ctest --preset debug-gcc
cmake --preset debug-clang && cmake --build --preset debug-clang && ctest --preset debug-clang
```

## 在其他项目中使用

### 源码集成

```cmake
add_subdirectory(uvco)          # 或 FetchContent_MakeAvailable(uvco)
target_link_libraries(app PRIVATE uvco::uvco)
```

主项目默认开启 `UVCO_BUILD_TESTS` / `UVCO_ENABLE_INSTALL`；作为子项目两者默认关闭，
可显式开启。保留 CMake 3.14 下限（旧版本使用 top-level 判定兼容分支）。

### 安装包

所有安装路径由 GNUInstallDirs 控制。下面的示例只写入项目 `build/`：

```sh
cmake -S . -B build/package -DUVCO_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/prefix"
cmake --build build/package
cmake --install build/package
# 仅当 co2 是本次 FetchContent 引入时，单独安装它：
cmake --install build/package/_deps/co2-build --prefix "$PWD/build/deps"
```

`cmake --install` 需 CMake ≥3.15；3.14 可改用
`cmake -DCMAKE_INSTALL_PREFIX="$PWD/build/prefix" -P build/package/cmake_install.cmake`。
消费者的 CMakeLists.txt：

```cmake
find_package(uvco 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE uvco::uvco)
```

通过 `CMAKE_PREFIX_PATH` 指定 uvco/co2 安装前缀。例如运行项目附带的两 TU 消费者：

```sh
cmake -S tests/consumer -B build/consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/prefix;$PWD/build/deps"
cmake --build build/consumer
(cd build/consumer && ctest --output-on-failure)
```

依赖和兼容性决策：
- uvco 只安装自己的头文件与 CMake 包，不捆绑或自动安装 FetchContent 依赖。
  消费时必须已有可被 `find_package(co2 CONFIG)` 找到的 co2；预览版不传数字版本，
  在消费者配置阶段重新编译探测 co2 内部能力。
- 构建时通过 libuv CMake 包/父目标发现的依赖，安装包消费端使用 `find_dependency(libuv CONFIG)`；
  通过 pkg-config 发现的则寻找 PkgConfig 与 `libuv>=1.40`。所有路径都编译检查实际头文件版本。
  FetchContent libuv 的安装包消费者要求另行安装 libuv CMake 包；不在消费端自动下载。
- 版本文件采用 `SameMinorVersion`：0.x 不承诺跨 minor 兼容，同 minor 的 patch 遵循 CMake 版本规则。
  包可搬迁；目标不会记录原源码/构建目录。`uvco::uvco` 在源码和安装模式下名称一致。
- 非 imported 的源目标探测支持普通/BUILD_INTERFACE include 及传递依赖；复杂 generator expression
  会明确拒绝，避免误用系统头文件，需改用依赖的 installed/imported target。

## 示例

```cpp
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>

auto work(uvco::Loop& loop, co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (loop, pool)) {
    CO2_AWAIT(co2::scheduleOn(pool));  // 去线程池算点东西
    CO2_AWAIT(co2::scheduleOn(loop));  // 回到 loop 线程后才能碰 libuv
    CO2_RETURN(42);
}
CO2_END

int main() {
    uvco::Loop loop;                   // 用户持有 Loop，绑定当前线程
    co2::ThreadPool pool{2};
    return uvco::run(loop, work(loop, pool)) == 42 ? 0 : 1;
}
```

错误以 `uvco::Result<T>` 返回（错误码即 libuv 的负 errno，`message()` 走 `uv_strerror`，
可转换为 `std::error_code`，category 名为 `"uv"`）；用法错误是契约违规而非异常。


## 子进程

包含 `<uvco/process.hpp>`。`ProcessOptions::args` 不含 argv[0]；`spawn(loop, options)` 返回
`Result<Process>`，`CO2_AWAIT_SET(status, process.wait())` 得到退出码/信号。
`ProcessStdio::createPipe(pipe, childReads, childWrites)` 使用现有 `Pipe` 作为父端；方向按子进程视角。
取消 wait 不会杀子进程；必须显式 `kill(signum)`。析构交给后台 reap-only 控制块；close 等待退出并回收，
不再丢失回收责任。只有显式 detach() 移交回收责任。Loop 析构也等非 detach 子进程退出，可能无限阻塞；
必要时先 kill 再 close。完整生命周期语义见设计文档 §4.9。

生命周期约束：关闭 fd 不可取消且 awaiter 自构造持有所有权；handle awaiter 跨 close 安全；
Loop 析构中 Sleep 返回 aborted；fs 回调异常安全并明确取消线性化点；co2 内部接口集中兼容层及能力探测。


Linux 专用生命周期/泄漏测试按平台注册；子进程基本功能使用项目内编译的 helper，不依赖 /bin 命令。


whenAny 异常安全：分支结果取出/移动/构造抛异常时，先取消其余分支并等待所有已启动分支结束，
再在 await_resume 重抛；落败者的异常也不会丢失，嵌套 whenAny 与 timeout 遵循相同规则。
忽略取消且永不结束的分支仍会阻塞整个组合器。

## Result-aware all、DNS 与线程池工作

- `<uvco/sync.hpp>`：`whenAllResults(a, b, ...)` 接受 Task 或其他 awaitable，返回
  `Result<tuple<成功值...>>`，省略 void；`vector` 重载返回 `Result<vector<T>>`，void 范围返回
  `Result<void>`，空范围成功。首个错误/取消/异常请求停止兄弟，所有已启动分支完成后才交付；
  优先级为异常 > 首个业务错误 > 取消。分支左至右启动，失败后尚未启动的分支直接销毁。
  `co2::whenAll` 完全不变，仍把 `Result<T>` 当作值。
- `<uvco/dns.hpp>`：`getaddrinfo(loop, host, service, hints)` 返回有序的
  `Result<vector<SocketAddress>>`；字符串按值持有，hints 仅复制四个输入标量。
  `getnameinfo(loop, address, flags)` 返回 `Result<NameInfo>`（host/service 字符串）。
  DNS 的取消以回调入口检查 stop_token 为线性化点：此时已取消则释放并丢弃结果；之后取消不推翻完成。
  native addrinfo 在恢复协程前释放；投影 bad_alloc 返回 ENOMEM，其他异常清理后重抛。
- `<uvco/work.hpp>`：`queueWork(loop, fn)` 返回 `Result<R>`；fn 可接收一个 `co2::stop_token`
  用于协作轮询，也可无参。排队取消先 uv_cancel，成功则不运行 fn；运行中不强制中断，仍等 after-work。
  callable 一旦完成就返回实际结果（包括 void），晚到取消不丢弃结果；worker/结果构造异常保存 exception_ptr，清理后在 loop 重抛，
  优先于取消。内部取消 hook noexcept；若 fn 自己注册 token callback，也必须 noexcept。
  返回 canceled 不回滚已经发生的副作用。libuv 全局线程池由 DNS/fs/work 共享。

`detail::WaitSlot<T>` 已逐模块用于 Timer、stream read/accept、UDP recv、Process wait；
只统一等待者预约与结果交付，不改变 HandleRef、底层停止方式、opBegin/opEnd 或 inline/deferred 恢复策略。
单等待者预约直到 await_resume 取走结果后才释放，包含已完成但尚未恢复的阶段。

这些操作的错误仍走 Result；用户 callable/值构造抛出的 C++ 异常在请求完成后重抛。
忽略协作取消且永不结束的 Work/组合子分支仍会阻塞 drain；Process 默认回收策略未改变。

## 可选适配器与扩展

- `capture(loop, ProcessOptions, CaptureOptions)` / `communicate(...)`：同时写 stdin、读 stdout/stderr、
  等待退出，返回 `Result<CaptureResult>`（status/stdoutData/stderrData）。input 按值持有；
  stdoutLimit/stderrLimit 默认各 1 MiB，超过返回 ENOBUFS，不交付部分输出。
  `CaptureCancel::{stopWaiting,killChild,killGroup}` 默认为 stopWaiting：关闭本次 stdio、取消等待，
  子进程继续交给既有后台 reaper；kill 模式发送 killSignal（默认 SIGTERM），再等直接子进程回收。
  POSIX group 模式自动设置 detached 创建新 session/group；Windows 返回 ENOSYS。
  已回收 leader 不再寻址旧 PGID，避免误伤复用的标识；因此不保证清理 leader 退出后的后代。
  不提供信号升级，忽略 killSignal 的子进程仍可能阻塞；需要强制终止时显式选 SIGKILL（平台支持时）。
- `<uvco/stream.hpp>` 的 `BufferedReader<Stream>` 为可选适配层：
  `readExactly(n)`、`readLine(maxLength)`、`readToEnd(maxBytes)` 都返回 `Result<string>`。
  readLine 不含 LF、保留 CR，EOF 交付末尾未换行行，再返回 eof；exact 短 EOF、取消或 cap 错误
  保留未消费缓冲，可以用更大的 cap 重试。内部最多一次额外 4096 字节 read-ahead；
  cap 是输出限制，不是全局 allocator/缓冲容量限额。reader/stream 必须活过操作，不得并发或绕过 reader 直接读。
- `Tcp::open(loop, uv_os_sock_t)` / `Pipe::open(loop, uv_file)`：成功才移交 fd 所有权；失败由调用者
  保留并关闭。调用者须保证描述符类型正确、且未被其他 handle 拥有；libuv 不一定拒绝错误类型或
  非活跃句柄重复接管。失败不承诺恢复 nonblocking 等 flags。
- `TcpListener` / `PipeListener`：仅 bind/listen/accept/close 与本地地址查询的薄门面；accept 仍返回 Tcp/Pipe。
  旧 Tcp/Pipe API、控制块和零缓冲 stream core 不变。
- `fs::openDir(loop,path)` → `Result<fs::Dir>`；`dir.readDir(batch=64)` / `fs::readDir(dir,batch)`
  每次复制最多 batch 条（1..64），空 vector 表示结束。Dir 最后一个 lease 析构同步 closedir，
  `dir.close()` 也同步、不可取消，要求没有在途 read。未启动操作保活状态；显式 close 后它们返回 EBADF。
  openDir 复用 FsOp 的迟到取消释放；readDir 消耗游标，迟到取消仍交付真实读取结果，不能回滚游标。
- `yieldNextTurn(loop)` → `Result<void>`：跨至少一个完整 libuv iteration，任意调用 phase 都成立；
  支持取消和 Loop teardown（aborted），不改变 `scheduleOn(loop)`。
- UDP 新增 multicast/source membership、loop/TTL/interface 与 sendQueueSize/Count；
  选项和不支持平台错误透传 libuv，不增加接收队列或改变 UDP 完成策略。

whenAny range 现在仅用一个固定 Entry 数组保存 slot/end/exception/child，加原有 Task vector；
仍收集每个分支，loser 的 throwing move 继续传播，**没有做只提取赢家的语义修改**。
libuv 导入集中到内部 detail/libuv.hpp，加入 Windows guards 和 macro-resistant numeric_limits 写法；
这不等于已经完成 Windows/macOS 运行验证。

## 文件变更监控

```cpp
// fallible factory：含 init/start 的错误，不会关闭未初始化的 native handle。
auto watched = uvco::fs::FsEvent::watch(loop, directory); // Result<FsEvent>
// 检查 watched 后，在协程中：
CO2_AWAIT_SET(event, watched->next()); // Result<FsEventInfo>{owned filename, events}
// events 是 UV_RENAME / UV_CHANGE 位掩码；filename 不可确定时为空。
// watched->watch(newPath, flags) 事务式重启；失败保留旧监控。
// watched->stop() 中止等待，CO2_AWAIT(watched->close()) 等待安全关闭。

auto polled = uvco::fs::FsPoll::start(loop, filename, std::chrono::milliseconds{1000});
// 检查 polled 后：
CO2_AWAIT_SET(change, polled->next()); // Result<FsPollChange>{prev/curr uv_stat_t 副本}
// polled->setInterval(2s) 或 polled->start(newPath, 2s) 重启；interval() 查看设置。
```

`next()` 单等待者；从 awaiter 构造起保活控制块。stop_token 只取消本次等待（canceled），
不停止监控、不消耗缓存；stop/close/成功重启/~Loop 中止挂起等待（aborted，经 loop 延后恢复）。
原生回调内联恢复；先在 loop 上完成者胜出，迟到的取消不覆盖已交付结果。
空/移走/已 close 对象只能析构或 close；Loop 先析构后同样只允许 close/析构。
有挂起 next 时直接析构所有者仍是契约违规，请先 stop/close 并等待读者结束。

默认固定 **32 条** FIFO；相邻同名 FsEvent 合并 rename/change 位；FsPoll 成功快照不合并，保留顺序。相邻相同错误合并；
满时丢最旧条目。`queued()` / `overflowCount()` / `coalescedCount()` 可观测（计数饱和不回绕）。
stop 清空缓存但保留计数；成功重启清空缓存并归零计数。它不是可靠文件系统日志，
内核也可能合并/丢通知；容量限制条目数，不是 allocator 字节上限。
错误/投影异常也占队列条目，溢出时同样可能被丢弃。文件名复制 bad_alloc → ENOMEM，其他投影异常缓存并从 next 的 await_resume 重抛，绝不跨 C 回调抛出。

flags 使用 libuv 常量：默认 0；`UV_FS_EVENT_RECURSIVE` 仅 Windows/macOS 接受，Linux 等返回 ENOTSUP。
`UV_FS_EVENT_STAT` / WATCH_ENTRY 上游没有实现，本封装明确返回 ENOTSUP，**不隐式降级为轮询**。
不支持的后端（例如 QNX 的 no-fsevents）由 watch 工厂返回 ENOSYS，见 [QNX 说明](docs/qnx.md)。
`FsPoll` 用 stat 轮询；已有文件的首次成功 stat 只建立基线，不产生通知。
缺失/无权限等回调状态以 Result 错误交付，监控不停止；错误原因或文件状态变化后继续通知。
成功时 prev/curr 均为按值副本，恢复后的 prev 可能是零快照或上次成功 stat。
interval 必须是 [1, UINT_MAX] 毫秒；跨文件系统推荐多秒间隔，短间隔不保证发现所有变化。
`setInterval/start/watch` 成功均开启新监控代次（重新建基线，期间变化不保证完整），
分配新控制块且旧 native 请求异步排空；失败保留旧监控/缓存/配置。这也避免旧 libuv fs_poll
在 stat 在途时对同一 native handle stop/start 引发旧上下文复活。uvco 不额外建线程或无界队列（stat 仍用 libuv 全局线程池，原生后端缓存由 libuv/OS 管理）。

FsEvent 不承诺被监控目标自身 rename/delete 后自动跟随或重新订阅，请按 backend 语义显式 watch 重建。
FsPoll 按路径持续 stat，缺失后可恢复；stop 不强制中断已提交的 stat，close/~Loop 可能等待底层 I/O。
active watcher 会保持原生 loop 存活，调用 drain 前须 stop/close（run 仍只驱动到根任务完成）。

## GitHub CI

`.github/workflows/ci.yml` 在 push / pull_request / 手动触发时运行，不自动发布或推送。
- 阻塞项：Ubuntu 24.04，GCC/Clang 各 Debug + Release、Clang ASan/UBSan、Clang TSan，
  加一个强制 FetchContent co2/libuv 的 job。apt libuv 必须 >=1.40，否则构建校验 SHA256 的 v1.48.0。
- 所有配置开启测试与 install/export，包括独立消费者、前缀搬迁和两 TU 链接运行。
  Fetch job 在测试前把两个依赖单独安装到 build/deps，消费者不依赖生产包捆绑依赖。
- macOS（brew libuv）和 Windows MSVC（FetchContent 静态 libuv）为 experimental，
  **job-level continue-on-error，不作为已验证平台或合并门槛**；须在真实 runner 稳定后再升级为阻塞项。
- Linux/macOS 使用 ccache；所有平台缓存固定版本依赖源码，不缓存 CMake build tree。
  action 固定到核对过的 commit SHA，token 仅 contents:read，checkout 不保留凭据。
  同 ref 的新运行取消旧运行；失败上传日志/CTest/CMake 配置诊断，保留 14 天。
- TSan 仅在一次性 GitHub runner 将 vm.mmap_rnd_bits 调整到 28，以兼容 shadow mapping；
  不关闭 sanitizer、不靠重试或过滤掩盖报告。本地机器未修改此设置。

QNX 有 libuv 上游移植，但不代表 uvco 已验证支持，详见 [QNX 核对记录](docs/qnx.md)。

## 统一 watcher 与 Signal

内部采用 typed native Backend + EventPort/HookSink + 编译期 delivery policy；
FsEvent/FsPoll 的 FIFO32 与 Timer 的累计/取消/关闭行为保持不变。
`Timer::next()` 是 `tick()` 的精确别名，`FsEvent::start()` 是成员 `watch()` 的别名。

```cpp
auto signal = uvco::Signal::create(loop); // Result<Signal>；初始停止
if (!signal || !signal->start(SIGINT)) { /* 处理 Result 错误 */ }
CO2_AWAIT_SET(event, signal->next());     // Result<SignalEvent>{signum,count,saturated}
```

Signal 计数是 libuv 回调数（不是内核发送数），按 watcher 独立归约、饱和。
成功 start/restart 新建代次、延后中止旧 next；失败保留旧监听；取消 next 不停止监听。
禁止 SIGPIPE、不可捕获/同步故障信号；SIGCHLD 只通知，不抢 Process 的 reaper。
信号 disposition 是进程级资源，不承诺与外部 sigaction 无损共存。
Windows 只接受可实际接收的 INT/BREAK/HUP/WINCH（受控制台限制；本地未验证）。
详见 [统一 watcher 设计](docs/design-watchers.md)。

### Loop phase：hook 优先

```cpp
auto hook = uvco::onPrepare(loop, [](uvco::PhaseEvent e) noexcept {
    // 原生 prepare 阶段；短小、noexcept；不嵌套 run/销毁 Loop
}); // Result<PhaseHook<...>>；已启动、默认 unref

auto idle = uvco::onIdle(loop, [](uvco::PhaseEvent) noexcept { /* ... */ });
if (idle) idle->start(); // Idle 必须显式启动；即使 unref 仍可能让 poll 空转

auto check = uvco::Check::create(loop); // 高级适配，初始停止、默认 ref
if (check) check->start();
CO2_AWAIT_SET(seen, check->next());     // latest-only，延后恢复；不是 next-turn 保证
```

`onPrepare/onCheck` 自动启动；`onIdle` 保守地保持停止。
Hook 按值持有 noexcept callable，无 std::function/等待者/每事件分配；用 stop/close/RAII 停止。
高级 `Idle/Prepare/Check` 的 next 支持 stop_token、单等待者与共用诊断 stats。
Prepare/Check 的 ref 不会唤醒阻塞 poll，真正跨轮继续用 `yieldNextTurn`。

### TTY / console

```cpp
auto tty = uvco::Tty::duplicate(loop, fd); // Result<Tty>，不接管原 fd，失败也不关闭它
// 检查 tty 后：read/write/tryWrite/shutdown 沿用零缓冲 stream 语义；没有 listen/accept。
auto mode = tty->setMode(uvco::TtyMode::Raw); // Normal / Raw / Io（Windows Io 可返回 ENOTSUP）
auto size = tty->getSize();                 // Result<TerminalSize>{width,height}
// 等挂起的 read 完成后，可以显式 restore() 检查恢复错误。
```

setMode/restore 在 read 的预约尚未释放时返回 busy；close 与 ~Loop 在 native close 前尽力恢复 Normal。
本层不调用全局 uv_tty_reset_mode；Normal 不是对任意外部修改的精确快照恢复，mode 管理须由应用独占，
重叠 raw 区间不能由多个 owner 各自恢复。原生模式切换可能阻塞，close 恢复失败无法在析构中报告。
Unix reopened native fd 与辅助 duplicate 分别跟踪；fallback dup 的文件状态/终端模式仍可能共享。
重定向 stdio 返回 ENOTTY，请使用 Pipe/File；Windows 输入/输出 console 的能力与尺寸查询遵循 native 限制。
Unix PTY 覆盖双向 I/O、模式、取消/close/Loop-first、失败所有权与 fd leak；Windows 实机尚未验证。

Windows Job 与精确 ModeGuard 尚未实现。


### DNS resolver 与 Happy Eyeballs

```cpp
uvco::DnsCacheOptions cache; // enabled=false；默认不缓存，仍合并在途同 key 请求
cache.enabled = true;       // 应用主动选择；maxAge 是本地年龄，不是 DNS TTL
uvco::DnsResolver resolver{loop, cache};
CO2_AWAIT_SET(addresses, resolver.resolve("127.0.0.1", "80")); // Result<AddressList>
// AddressList = shared_ptr<vector<SocketAddress> const>；可越过 resolver 生命周期持有
CO2_AWAIT_SET(tcp, uvco::connect(loop, resolver, "example.test", 443)); // Result<Tcp>
```

缓存按 host/service + hints 标量 + invalidation 代次精确匹配，成功结果 LRU，默认上限
256 条 / 估算 4 MiB、32 个在途 producer / 256 个订阅者；不缓存失败。
`invalidate()` 切断旧代次（旧订阅仍可完成，但不回写新缓存）；单个 stop_token 只解除该订阅，最后一个订阅离开即请求取消 producer。
`close()` / ~Loop 先 aborted 订阅者，再保证 producer drain；在途 producer 用 resident/native async ref
保活，**不占外部 Loop::KeepAlive**。系统 NSS 或不配合取消的注入 producer 仍可能延迟关闭；
注入 backend 必须最终完成/响应 teardown。缓存字节预算不包含调用者持有的共享结果/allocator 开销。

`ConnectOptions`：resolutionDelay=50ms、attemptDelay=250ms、minimumDelay=100ms、
maxConcurrent=4（1..64）、perAttemptTimeout=5s、overallTimeout=30s；minimumDelay 不小于 10ms，
attemptDelay 不小于 minimumDelay 且不大于 2s。
数字地址绕过 DNS；双族并行解析，族间交错，普通连接失败继续比赛；首个成功取消并 drain 连接落败者，
但不等待独立 DNS producer。全败取最后发起候选的错误；异常 drain 后重抛。
`addressPolicy` 只在族内排序；owned `connector` / `clock` 供确定性测试和高级策略注入。
connector/clock/policy 也遵循 loop-thread 契约；connector 应使用可由 uvco teardown 的 awaiter，
不能留下脱离 loop 生命周期的外部任务。
这是 RFC8305 **核心子集**，不宣称完整 RFC6724、NAT64 合成或完整 RFC8305 合规。

### POSIX process group（best effort，不冒充严格 kill-tree）

```cpp
uvco::ProcessOptions options;
options.file = "worker";
options.privateGroup = true;               // POSIX 新 session/group；仍由 Process 回收
// 检查 spawn 的 Result 后：
auto sent = process.signalGroup(SIGTERM);   // Result<void>；0 只探测，负数非法
```

只有 privateGroup 或原有 detached 建立的组可使用；普通 Process 返回 ENOTSUP。
`waitid(... WNOWAIT)` 非回收检查后才能寻址 PGID；leader 已退出/被回收返回 ESRCH，
包括 libuv 已回收但 exit callback 尚未交付的窗口。要求 loop 线程、libuv 唯一 reaper；
不支持外部 waitpid / SIGCHLD 所有权竞争，不使用 kill(pid,0) 作身份保证。
`CaptureCancel::killGroup` 共用此安全路径。默认 reap-on-destruct 不变；不自动升级信号，
leader 退出后不保留“稍后再 SIGKILL 同一 PGID”的权限。后代可 setsid/setpgid 逃逸；
缺少 WNOWAIT 的平台及 Windows Job 未实现路径返回 ENOTSUP；Windows capture killGroup 沿用 ENOSYS。




## 构造与失败规则

推荐对资源边界使用 `Tcp::create(loop)`、`Udp::create(loop)`、`Timer::create(loop)`、
`DnsResolver::create(loop, options)`：返回 `Result<T>`，保留 native Errc，内部 bad_alloc 映射 no_memory。
现有 FsEvent/FsPoll/Signal/phase/TTY 的工厂保持同一风格。

保留三种明确的构造方式，不默默改变旧调用的失败语义：
1. `Tcp{loop}` / `Udp{loop}` / `Timer{loop}`（及旧 Pipe）是 legacy convenience：分配可能抛 bad_alloc，native 初始化失败为契约违规。
2. `DnsResolver{loop,...}` 是 throwing convenience：分配抛 bad_alloc，native 错误抛带 uv_category 的 system_error；不再伪装成 OOM。
3. `create/watch/open/spawn` 等 Result factory 用于需要处理资源失败的代码。参数值本身的 C++ 构造/复制仍可能抛异常。
空 owner 仅表示未绑定，不代表初始化失败；不得对空 owner 发起 I/O。

## 写入、工作与调度边界

- pending write 收到 stop 时默认关闭整个流，native write 回调完成后返回 canceled；同流其他操作可能 aborted。
  预取消不关闭流，已完成结果不被晚到取消覆盖，显式 close 与 token 取消仍区分 aborted/canceled。
- write 不是事务：错误前可能已发送前缀。部分 try_write 后提交失败/分配失败，或 queued write 回调失败时关闭流。
  Result<void> 不报告偏移，禁止向原流盲目重试整包；需要偏移的代码自行管理 tryWrite。
- Work 只有成功 dequeue 的取消保证 callable 不运行；运行中 token 用于协作，函数完成就交付值/void，异常排空后重抛。
- 双族 DNS 均失败时优先保留 EAI_AGAIN，永久 miss 不掩盖可重试故障；没有 transient 时优先非 miss，最后稳定按 IPv6。
- `Loop{scheduleCapacity}` 默认每个队列 65536 项，构造时预分配 local/batch/remote/remoteBatch（64位约2MiB）。
  noexcept schedule/drain 不分配、不丢 continuation、不内联重入；超过显式容量是契约违规，可在构造时提高容量。
  `Loop` 构造期 bad_alloc 在 native 初始化前发生，不遗留 native 资源。

## 项目信息

[MIT License](LICENSE) · [Changelog](CHANGELOG.md) · [验证记录与平台范围](docs/verification.md) · [Review 修订](docs/review2.md)

### KCP（实验性，默认关闭，尚未完成验收）

`-DUVCO_ENABLE_KCP=ON` 构建可选静态 target `uvco::kcp`；使用者显式链接该 target 并包含
`<uvco/kcp.hpp>`，安装包支持 `find_package(uvco CONFIG REQUIRED COMPONENTS kcp)`。
原有 `uvco::uvco` 不自动启用 KCP。

已实现纯 `KcpCore`（注入时间）、`KcpEndpoint::create/openSession/openStream`、
消息 `KcpSession::sendMessage/recvMessage/drain`、字节流 `KcpStream::write/readSome/drain`，
以及有界 `acceptPeer` 候选与显式 `openSession(std::move(candidate))`。未知 conv 不自动创建会话。
`sendMessage` 成功只代表进入本地可靠队列；stream write 可短写，调用者根据返回字节数推进。
KCP 没有 FIN/握手/认证；关闭是本地操作，非远端 EOF。取消 pending waiter 不撤销已提交消息。

**当前阻塞**：原样上游 `IOFFSETOF` 空指针成员取址宏触发 UBSan；不是已批准的 OOM 限制。
没有屏蔽报告或修改 vendor。Linux KCP CI、benchmark 和完整寿命回归仍待完成，勿视作生产可用。
见[设计与新审批点](docs/design-kcp.md#14-实现进度与新审批点)、[验证记录](docs/verification.md)。
