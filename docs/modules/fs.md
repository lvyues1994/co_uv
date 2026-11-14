# 模块：文件系统与 fs watcher

## 职责

在 libuv 线程池上提供异步文件与路径操作，以及目录迭代；并用统一 watcher 内核提供 `FsEvent`（内核通知）和 `FsPoll`（定时 stat）。

头文件：`<uvco/fs.hpp>`。命名空间 `uvco::fs`。

## 公开 API

### File 与路径操作

| 名称 | 说明 |
| --- | --- |
| `File(loop, fd)` | 接管已打开 fd。只可移动。析构若仍打开则**同步** `uv_fs_close`（无回调）。 |
| `read` / `write` / `sync` / `truncate` / `stat` | 返回对应 `*Op` awaiter。`offset=-1` 表示从当前位置。 |
| `close()` | File 立即变空，awaiter 独占 fd。**不可取消**；未被等待的 awaiter 析构时同步关闭。 |
| `release()` / `reset()` / `fd()` / `isOpen()` | 放弃所有权、同步关、查询。 |
| `open` / `stat` / `lstat` / `unlink` / `mkdir` / `rmdir` / `rename` / `copyfile` | 路径操作，返回 `*Op`。 |
| `mkdtemp` / `mkstemp` / `readlink` / `realpath` / `scandir` / `sendfile` | 其余路径与发送文件。 |
| `OpenOp`、`StatOp` 等 | 可放在 `CO2_BEG` 局部列表里以左值等待，零额外分配；右值便捷函数会因 `uv_fs_t` 约 440 字节而装箱一次。 |
| `readFile` / `writeFile` | `Task<Result<string>>` / `Task<Result<void>>`，内部使用帧局部 awaiter。writeFile 默认创建或截断。 |

产生资源的操作（open、mkdtemp 等）在迟到取消时会释放已产生的资源。不产生资源的操作（如 unlink）报告真实结果，副作用已发生。

### Dir

| 名称 | 说明 |
| --- | --- |
| `openDir(loop, path)` | `Result<Dir>`。 |
| `dir.readDir(batch=64)` / `readDir(dir, batch)` | 每次最多复制 batch 条（1..64）；空 vector 表示结束。 |
| `dir.close()` | 同步 `closedir`，不可取消；要求没有在途 read。显式 close 后未启动的操作返回 `EBADF`。 |

最后一个 lease 析构也会同步 closedir。`openDir` 复用 FsOp 的迟到取消释放；`readDir` 消耗游标，迟到取消仍交付真实读取结果，不能回滚游标。

### FsEvent / FsPoll

| 名称 | 说明 |
| --- | --- |
| `FsEvent::watch(loop, path, flags=0)` | 工厂。flags 仅允许 libuv 已知位；`WATCH_ENTRY`/`STAT` 当前拒绝，`RECURSIVE` 为 `ENOTSUP`。QNX 等无 fsevents 的后端返回 `ENOSYS`。 |
| `watch` / `start` / `stop` / `next()` | `next()` → `Result<FsEventInfo>`（`filename` 为相对名，可能空；`events` 为 `UV_RENAME\|UV_CHANGE` 位，不是操作日志）。32 项 FIFO，同名合并事件位。 |
| `FsPoll::start(loop, path, interval=1000ms)` | 定时 stat。`next()` → `Result<FsPollChange>`（前后 `uv_stat_t` 拷贝）。路径内嵌空字符或 interval≤0 为 `EINVAL`。 |

成功的再次 `watch`/`start` 开启新纪元。`stop`/`close` 使挂起的 `next()` 为 `aborted`。

## 语义

- 路径/File 操作走线程池，取消通常 `uv_cancel`；已在执行时可能 `EBUSY`，等自然完成。
- 回调入口是取消线性化点之一；投影异常会清理后重抛，`bad_alloc` 多为 `ENOMEM`。

## 内部要点

FsEvent/FsPoll 使用 `BoundedFifo<32>`。ENOSYS 的 FsEvent 不会对未初始化句柄调用 `uv_close`。

## 限制

- 同步析构/close 可能阻塞 loop 线程（尤其是网络文件系统）。
- Linux 上 libuv 可能把 fs 交给 io_uring，`uv_cancel` 行为与线程池不同；测试里部分用例会关 io_uring。
- 平台事件语义（文件名、递归、重命名）以 libuv/OS 为准，不是可移植日志。
