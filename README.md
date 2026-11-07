# uvco

[![CI（OWNER/REPO 占位）](https://github.com/OWNER/REPO/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/REPO/actions/workflows/ci.yml)

> 仓库尚未发布；发布到 GitHub 后将 badge 的 `OWNER/REPO` 替换为实际路径。

基于 [co2](https://github.com/lvyues1994/coro)（C++14 无栈协程库）对 libuv 的协程封装。
设计见 [docs/architecture.md](docs/architecture.md)。

当前进度：v0.1 第 1 步 **core**（`Result<T>`、`Loop`、`run()`、`detail::ReqAwaiter`）、第 2 步 **timer**（`sleep()`、`Timer`、通用句柄生命周期）、第 3 步 **sync**（`Event`、`Mutex`、`Channel<T>`、`whenAny`、`timeout`）、第 4 步 **stream**（`Tcp`、`Pipe`：拉模式 read、write、shutdown、listen/accept、connect）、第 5 步 **udp**（`Udp`：拉模式 recv 返回 `Datagram`、send、connect）、第 6 步 **fs**（`File`、`open`/`stat`/`unlink`/`mkdir`/`rename`/`mkstemp`/`scandir`/`copyfile`/`sendfile` 等、`readFile`/`writeFile`，迟到取消不泄漏 fd）。
v0.1 范围（文档 §5）已全部实现；另已增加 **process**（`spawn`、可取消 `wait`、`kill`、stdio Pipe）；现已增加 **dns**（`getaddrinfo/getnameinfo`）、**work**（`queueWork`）与 Result-aware `whenAllResults`；signal/tty/poll 留待后续。

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

Review 修复：关闭 fd 不可取消且 awaiter 自构造持有所有权；handle awaiter 跨 close 安全；
Loop 析构中 Sleep 返回 aborted；fs 回调异常安全并明确取消线性化点；co2 内部接口集中兼容层及能力探测。


此前 review 修复验证（当时 21 个 CTest 项）：g++/clang++ 各重复 5 遍、ASan/UBSan 10 遍、TSan 20 遍通过。
Linux 专用生命周期/泄漏测试按平台注册；子进程基本功能使用项目内编译的 helper，不依赖 /bin 命令。


whenAny 异常安全：分支结果取出/移动/构造抛异常时，先取消其余分支并等待所有已启动分支结束，
再在 await_resume 重抛；落败者的异常也不会丢失，嵌套 whenAny 与 timeout 遵循相同规则。
此前 whenAny 修复新增 19 个异常回归用例；当时全套 22 个 CTest 项在 g++/clang++（各 5 遍）、ASan/UBSan（10 遍）、
TSan（20 遍）通过。忽略取消且永不结束的分支仍会阻塞整个组合器。

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
  取消完成线性化点同样是 after-work 入口；worker/结果构造异常保存 exception_ptr，清理后在 loop 重抛，
  优先于取消。内部取消 hook noexcept；若 fn 自己注册 token callback，也必须 noexcept。
  返回 canceled 不回滚已经发生的副作用。libuv 全局线程池由 DNS/fs/work 共享。

`detail::WaitSlot<T>` 已逐模块用于 Timer、stream read/accept、UDP recv、Process wait；
只统一等待者预约与结果交付，不改变 HandleRef、底层停止方式、opBegin/opEnd 或 inline/deferred 恢复策略。
单等待者预约直到 await_resume 取走结果后才释放，包含已完成但尚未恢复的阶段。

这些操作的错误仍走 Result；用户 callable/值构造抛出的 C++ 异常在请求完成后重抛。
忽略协作取消且永不结束的 Work/组合子分支仍会阻塞 drain；Process 默认回收策略未改变。

P1 最新验证：27 个 CTest 项（含公共头独立包含构建），g++/clang++ 各重复 5 遍，
ASan/UBSan + leak detection 重复 10 遍，TSan 重复 20 遍通过。
新增 allResults、DNS、Work、WaitSlot 用例覆盖错误/取消/异常/并发与资源释放；Linux DNS
故障注入通过 linker wrap 保留 sanitizer 的 allocator interceptor，不关闭 sanitizer。
P1 当时未覆盖 install/export（现已完成，见使用章节）；Windows/macOS、DNS 缓存/Happy Eyeballs、强制中断 worker 仍未覆盖。

## P2：可选适配器与扩展

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

P2 验证（Linux）：38 个 CTest，g++ / clang++ 各重复 5 遍，ASan/UBSan + leak detection
重复 10 遍，TSan 重复 20 遍，共 1520 次全部通过；全部公共头独立编译通过。
新增覆盖双输出并发 + 大 stdin、独立 cap、pending write 取消、输出异常 drain/reap、
BufferedReader 取消重试、fd adoption 成败/占用冲突、目录 lease/迟到取消/投影异常、
yield 五种 phase/预取消/Loop teardown、UDP 选项与 listener。
构建与测试日志位于 `build/p2-final-{gcc,clang,asan,tsan}-*.log`。

install/export 最新验证（Linux）：40 个 CTest 在 g++、clang++、ASan/UBSan（含 leak detection）、
TSan 四构建各重复 2 遍，共 320 次全部通过；安装/源码消费者也继承对应 sanitizer flags。
额外通过 CMake 3.28 的安装与两 TU 消费者测试；主验证使用 CMake 4.4。
日志见 `build/install-{gcc,clang,asan,tsan}-*.log`；CMake 3.14、Windows/macOS 和多配置生成器未实机验证。

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

本地验证方法：`build/ci-tools/actionlint .github/workflows/ci.yml`（工具不提交）。
本轮本地通过 GCC/Clang 的 Debug/Release、ASan/UBSan、TSan，六配置各 40 个 CTest；
另通过强制 FetchContent 发现路径的真实 libuv/co2 构建、独立安装及 39 个 CTest（使用本地 source override，
libuv 源自校验过的官方 v1.48.0 归档，不测试 Git 下载）。actionlint 1.7.12 检查通过。
Release 检查发现 fs 测试的替代 new/delete 被 GCC 优化误报，单独用 noinline 保持 hook 边界，
未放宽 -Werror 或关闭 sanitizer。日志为 build/ci-local-*-{config,build,test}.log；红例 build/ci-gcc-release-red.log。
GitHub runner、macOS、Windows 及全新 Git 下载尚未运行；以上不是远程 CI 已绿的声明。
QNX 有 libuv 上游移植，但不代表 uvco 已验证支持，详见 [QNX 核对记录](docs/qnx.md)。
