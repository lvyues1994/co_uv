# 模块：Process

## 职责

在 libuv 上 spawn 子进程、等待退出、发送信号，以及可选的 stdin/stdout/stderr 捕获。回收责任默认留在 uvco 一侧，避免把僵尸丢给调用者。

头文件：`<uvco/process.hpp>`。

## 公开 API

| 名称 | 说明 |
| --- | --- |
| `ProcessOptions` | `file`；`args` **不含** argv[0]（永远是 file）；`env` + `inheritEnv`（空 env 且 inheritEnv=false 表示空环境）；`cwd`；`stdio`（默认忽略三路）；`detached`；`privateGroup`（POSIX 新 session/group）；`setUid`/`setGid`。 |
| `ProcessStdio::ignore()` / `inherit(fd)` / `inherit(Pipe&)` / `createPipe(pipe, childReads, childWrites)` | 方向按**子进程**视角。父端 Pipe 须在同一 Loop 上已初始化且尚未打开对端。 |
| `spawn(loop, options)` | `Result<Process>`。 |
| `wait()` | `Result<ExitStatus>`。`ExitStatus::{exitStatus, termSignal}`。同时只能有一个 wait。 |
| `kill(signal)` | 向该子进程发信号。已退出为 `ESRCH`，正在关闭为 `aborted`。 |
| `signalGroup(signal)` | POSIX 且 `privateGroup`（或 spawn 时 detached 导致的私有组）时，向进程组发信号。不是严格 containment。Windows 为 `ENOTSUP`。leader 已退出/回收后拒绝旧 PGID。缺少 `WNOWAIT` 时可能 `ENOTSUP`。 |
| `detach()` | 停止监视并把 OS 回收责任交给调用者，返回 pid。 |
| `communicate(loop, options, io = {})` | `Task<Result<CaptureResult>>`。同时写 stdin、读 stdout/stderr、等退出。 |
| `CaptureOptions` | `input` 按值；`stdoutLimit`/`stderrLimit` 默认各 1MiB；`cancel` 默认 `stopWaiting`；`killSignal` 默认 SIGTERM。 |
| `CaptureCancel::{stopWaiting, killChild, killGroup}` | 取消时的行为。 |
| `CaptureResult` | `status` / `stdoutData` / `stderrData`。 |

## 语义

- **析构**：若子进程仍在跑，交给后台 **reap-only** 控制块（仍 ref 住 loop），**不发信号**。`close()` 中止挂起的 wait，并等到退出后才完成关闭。
- **detach()**：唯一把回收责任交给调用者的路径。
- **`~Loop`**：等待所有未 detach 的子进程退出，**可能永远阻塞**。需要时先 `kill` 再关闭。
- **取消 wait**：只结束这次等待，**不杀**子进程。子进程继续由默认回收路径负责。
- **capture 取消**：
  - `stopWaiting`（默认）：关闭本次 stdio、取消等待，子进程交给既有 reaper。
  - `killChild` / `killGroup`：先发 `killSignal`，再等直接子进程回收。Windows 上 group 为 `ENOSYS`。POSIX group 会为 capture 自动走 detached 以创建新 session/group。已回收的 leader 不再寻址旧 PGID。超过输出上限返回 `ENOBUFS`，**不**交付部分输出。

## 内部要点

退出回调在 loop 线程把 `ExitStatus` 写入 WaitSlot。reap-only 块在 `onExit` 后自行关闭。stdio 的 `UV_CREATE_PIPE` 必须带方向位，否则 `spawn` 返回 `EINVAL`。

## 限制

- POSIX `signalGroup` 是 best-effort：后代可用 `setsid`/`setpgid` 逃逸。不是 Windows Job，也不是完整 kill-tree。
- 不提供信号升级；忽略 SIGTERM 的子进程可能挡住 close/`~Loop`。需要强制终止时显式选 SIGKILL（平台支持时）。
- capture 不保留超限时的部分输出。
