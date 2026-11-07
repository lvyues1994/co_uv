# libuv / uvco 的 QNX 状态

核对日期：2026-11-07（北京时间 / Asia/Shanghai）。这是源码/文档核对，没有运行 QNX SDK 或目标机测试。

## 结论与保障等级

**libuv 有 QNX 后端，当前上游包含 QNX 8 支持；但不是 Tier 1/2 平台保障。**
[SUPPORTED_PLATFORMS.md](https://github.com/libuv/libuv/blob/v1.x/SUPPORTED_PLATFORMS.md)
没有单列 QNX，因此按表中的 Other / Tier 3 理解：社区维护，可能意外损坏，没有指定 QNX 版本保障。
不要把“有实现”或 QNX port 的测试结果解释为主流 Linux/macOS/Windows 同级 CI 承诺。

- 最初移植：[PR #2991](https://github.com/libuv/libuv/pull/2991)，2020 年落入上游。
- QNX 8 的历史构建问题：[issue #4168](https://github.com/libuv/libuv/issues/4168)、
  [issue #4710](https://github.com/libuv/libuv/issues/4710)，涉及 cpuinfo/new_cpuinfo 等 SDK 差异。
- [PR #4801](https://github.com/libuv/libuv/pull/4801) 在 2025 年补充 QNX 8 与较早版本适配，
  已收录于 [libuv 1.52.0](https://github.com/libuv/libuv/releases/tag/v1.52.0)。
  uvco 当前默认 FetchContent **v1.48.0 早于这些修复**；最低 libuv >=1.40 不代表 QNX 8 可用。
  QNX 8 应选含修复的新版本（至少 1.52.0）/已验证 SDK port，再针对 uvco 实测；本轮不更改默认版本。

## 实现和功能边界

| 项目 | 核对结果 |
|---|---|
| 构建 | [CMake QNX 分支](https://github.com/libuv/libuv/blob/v1.x/CMakeLists.txt) 明确选择 qnx.c、posix-poll、posix-hrtime、bsd-ifaddrs、no-proctitle、no-fsevents，并链接 socket |
| 事件后端 | [posix-poll.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/posix-poll.c) 使用 poll，不是 Linux epoll/io_uring；io_uring 缺失不构成 QNX 移植问题，普通异步 FS 可用线程池路径 |
| fs_event | [no-fsevents.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/no-fsevents.c) 的 init/start/stop 返回 UV_ENOSYS。不是“QNX 有原生文件监控”；可考虑 fs_poll/stat 轮询（语义不同） |
| uv_spawn | QNX 编译通用 [unix/process.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/process.c)，不是 QNX ENOSYS stub；有 stdio、退出回调、SIGCHLD/waitpid 路径。具体 SDK、权限、进程组选项仍需目标机验证 |
| interface addresses | [bsd-ifaddrs.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/bsd-ifaddrs.c) 实现 getifaddrs / uv_interface_addresses，QNX CMake 已选用；**并非缺失功能**，v1.48.0 的 QNX 分支也已包含它 |
| 进程标题 | [no-proctitle.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/no-proctitle.c)：set 不做事，get 返回空串，不能依赖真实标题修改 |
| 系统信息 | [qnx.c](https://github.com/libuv/libuv/blob/v1.x/src/unix/qnx.c)：loadavg 固定零；内存查询使用 pidin info，RSS 使用 QNX /proc/self/ctl + devctl，依赖目标镜像提供这些设施 |

[QNX ports 构建/测试说明](https://github.com/qnx-ports/build-files/blob/main/ports/libuv/README.md)
提供 Linux host + QNX SDK 和目标机测试步骤，其 QNX8/RPi4 记录列出 ipc_tcp_connection 超时与
pipe_connect_to_file 两项已知失败。该记录是特定 port/测试环境的证据，不是当前上游所有 QNX 版本
必现的错误列表，也不能据此保证 uvco 的 process/capture/IPC 测试通过。

## 对 uvco 的影响

1. **还不能承诺开箱即用。** 需要 QNX SDK/q++、C++14、目标 libuv/co2，先做交叉配置/编译与目标机 smoke。
   当前 co2/libuv capability 探测为编译检查，不执行目标程序，有利于交叉构建；完整 CTest 消费者流程
   不是现成的 QNX 交叉运行 harness，初次移植应关闭 UVCO_BUILD_TESTS，另建目标机测试。
2. `tests/CMakeLists.txt` 将 fs_tests/process_tests、GNU linker --wrap fixtures 限于 Linux。
   `/proc/self/fd` 泄漏计数是 Linux 测试依赖，不是生产封装依赖；不能复制到 QNX，QNX 自有 procfs 也不同。
   POSIX/UNIX 测试仍需要逐项检查，不能把“Linux 测试被跳过”当成功能已经验证。
3. Process 委托 uv_spawn；应重点验证 Pipe stdio/capture、退出先于 wait、取消、SIGCHLD/reaper、
   detach/killGroup/uid/gid 与目标路径。默认 reap-on-destruct / Loop 可能等待子进程的语义不变。
4. `core/loop.hpp` 的 SIGPIPE 策略适用于所有非 Windows 平台：默认动作才改为忽略，已有用户 handler 保留。
   QNX 上同样可能生效，是进程级副作用；必须验证 EPIPE 与子进程信号行为，而非假定是 Linux-only。
5. 当前没有 QNX CI/SDK/设备。uvco 已增加 FsEvent：fallible `FsEvent::watch(loop,path)` 工厂
   将 init 的 ENOSYS 作为 Result 错误返回，不关闭未初始化 handle，不隐式替换为轮询；Linux 包装 init 的
   故障回归验证了清理路径，但不是 QNX 目标机测试。需要轮询时可显式选 `FsPoll::start(loop,path,interval)`；
   该模块使用公开 uv_fs_poll API 与 stat 副本，不依赖 inotify/procfs，仍需目标机验证。新加的 macOS/Windows CI 也只是非阻塞探路，不改变支持承诺。
