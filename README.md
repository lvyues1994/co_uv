# uvco

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

在其他项目中使用：

```cmake
add_subdirectory(uvco)          # 或 FetchContent
target_link_libraries(app PRIVATE uvco::uvco)
```

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
尚未验证 Windows/macOS；install/export、DNS 缓存/Happy Eyeballs、强制中断 worker 均不在本轮范围。
