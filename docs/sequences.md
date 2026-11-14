# uvco 关键时序

下列图描述现行实现的控制流，而不是抽象愿望。参与者名称与源码中的类型对应。模块语义见 [modules/](modules/README.md)，关闭与取消不变量见 [architecture.md](architecture.md)。

## 1. 左值 ReqAwaiter：fs::stat

帧局部 `StatOp` 就地等待：co2 只在 await 槽里存指针，请求体留在协程帧中。

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant Op as 帧局部 StatOp
    participant Co2 as co2 await 槽
    participant EvLoop as Loop 线程
    participant Pool as libuv 线程池

    App->>Op: CO2_AWAIT_SET(r, op)
    Co2->>Op: 就地 await_suspend（槽内只记地址）
    Op->>Op: stop 已请求则立即 ECANCELED
    Op->>EvLoop: uv_fs_stat，req.data 指向帧内对象
    EvLoop->>Pool: 排队执行
    Pool-->>EvLoop: 完成回调
    EvLoop->>Op: complete(status)
    Op->>App: resume，await_resume 得到 Result
    Note over App,Op: 之后可改路径再次等待同一个 op
```

右值 `CO2_AWAIT(fs::stat(loop, path))` 会把约 440 字节的 awaiter 移进槽，超出 64 字节时多一次堆分配；回调里的 `this` 是槽里那份副本，不是帧局部对象。

## 2. 热路径 write（try_write 成功）

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant WA as WriteAwaiter
    participant Uv as libuv

    App->>WA: CO2_AWAIT_SET(r, tcp.write(buf))
    WA->>Uv: uv_try_write
    Uv-->>WA: 全部写完
    WA-->>App: await_suspend 返回 false，结果成功
    Note over WA: awaiter 小于等于 64 字节，右值不等待也不堆分配
```

未写完的部分会分配原生 `uv_write` 请求，并在请求上挂 `CancelHook`。

## 3. stop_token 取消在途请求

以可取消的 fs 请求为例（`uv_cancel` 有效）。任意线程都可能 `request_stop`。

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant Hook as CancelHook
    participant EvLoop as Loop 线程
    participant Uv as libuv

    App->>Uv: start 请求，state=pending
    App->>Hook: arm(stop_token)
    Note over Hook: 另一线程 request_stop
    Hook->>EvLoop: post 回调（不在该线程直接 uv_cancel）
    EvLoop->>Hook: 在 loop 线程运行
    alt 请求仍 pending
        Hook->>Uv: Derived::cancel，通常是 uv_cancel
        Uv-->>EvLoop: 回调 status=ECANCELED
        EvLoop->>App: complete 后 resume，结果 canceled
    else 已经完成
        Note over Hook: 不再碰请求，先到者有效
    end
    App->>Hook: await_resume 里 disarm
```

已请求停止且尚未 start 时，ReqAwaiter 不发起请求，`await_suspend` 返回 false，结果 `canceled`。

## 4. timeout(write) 关闭流

`timeout(loop, d, tcp.write(buf))` 在计时器先到时取消 write；按现行契约，取消挂起的 write 会关闭整个流。

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant TO as timeout
    participant W as WriteAwaiter
    participant S as Sleep
    participant Tcp as Tcp 控制块
    participant Uv as libuv

    App->>TO: CO2_AWAIT timeout(loop, d, write)
    TO->>W: 先启动 write
    W->>Uv: try_write 未写完，uv_write 排队
    TO->>S: 再启动 sleep
    S-->>TO: 先到期
    TO->>W: 请求停止 write
    W->>Tcp: 取消路径关闭整个流
    Uv-->>W: 原生写回调
    W-->>TO: 结果 canceled
    TO-->>App: 映射为 timed_out
    Note over Tcp: 之后在该流上读写返回 bad_descriptor，只能 close
```

若 write 在睡眠前就完成，计时器不会启动，结果是写成功，流保持打开。

## 5. Loop 析构与关闭顺序

```mermaid
sequenceDiagram
    participant User as 调用方
    participant EvLoop as Loop
    participant H as uvco 句柄
    participant F as raw 外源句柄
    participant Uv as uv_run ONCE

    User->>EvLoop: 析构（须在 loop 线程，无 run，无 KeepAlive）
    EvLoop->>H: loopClosing / closeInternally
    H->>H: 挂起操作 aborted（常经 schedule 延后恢复）
    EvLoop->>F: uv_walk，uv_close
    loop 直到 loop 不再存活
        EvLoop->>Uv: uv_run ONCE
        Uv-->>H: close 回调、中止请求的回调、因此恢复的协程
    end
    EvLoop->>EvLoop: 关闭内部 async 与 idle，uv_loop_close
    EvLoop->>H: 仍存活者 loopGone，变为孤儿
    Note over H: 孤儿此后只能 close 或析构，不得再启动 I/O
```

未 detach 的 `Process` 会使这一阶段一直等到子进程退出。

## 6. Process：spawn、wait、析构回收

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant P as Process
    participant B as ProcessBlock
    participant Child as 子进程
    participant EvLoop as Loop

    App->>P: spawn(loop, options)
    P->>B: uv_spawn
    App->>P: CO2_AWAIT wait()
    alt 取消 wait
        App->>B: cancel，结果 canceled
        Note over Child: 子进程继续跑，回收责任不变
    else 子进程退出
        Child-->>B: onExit，写入 ExitStatus
        B->>App: resume wait，成功
    end
    alt 用户析构 Process 且子进程仍在
        P->>B: abandon：reap-only，不发信号
        Child-->>B: onExit 后自行 close
    else detach
        P->>P: 停止监视，调用者负责 waitpid
    else 事件循环析构
        EvLoop->>B: 等待未 detach 的子进程，可能一直阻塞
    end
```

## 7. Happy Eyeballs connect

核心子集：解析后按族交错、有节奏地并发有限个 `Tcp::connect`，不是完整 RFC 6724。

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant HE as connect 协调器
    participant R as DnsResolver
    participant W as 连接 worker
    participant Tcp as Tcp

    App->>HE: connect(loop, resolver, host, port)
    HE->>R: resolve 双族
    R-->>HE: 地址列表（可经 addressPolicy 在族内排序）
    HE->>HE: 交错 IPv6 与 IPv4，最多 64 个 worker
    loop 节奏窗口
        HE->>W: 启动下一个 connectOne
        W->>Tcp: connect(地址)
        alt 先成功
            Tcp-->>HE: 已连接的 Tcp
            HE->>HE: 请求停止其余 worker 并等待它们结束
            HE-->>App: Result 成功
        else 失败
            W->>Tcp: close 该套接字
            HE->>HE: 记录可重试错误，启动下一地址
        end
    end
    alt 全部失败或父取消
        HE-->>App: 错误或 canceled
    end
```

## 8. Watcher 事件投递（next）

以 FsPoll / Signal / Timer 这类 EventPort 为例。HookSink（`onPrepare` 等）不经队列，直接调用 `noexcept` 回调。

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant W as Watcher 句柄
    participant Port as EventPort
    participant Uv as libuv 回调

    App->>W: start 或 watch
    App->>W: CO2_AWAIT next()
    W->>Port: 预约单等待者
    Uv->>Port: publish(事件或错误)
    Port->>Port: 按编译期策略合并或入队
    alt 已有等待者
        Port->>App: 延后或按策略 resume，交付 Result
    else 无人等待
        Port->>Port: 按策略积压（Timer 累计，FIFO 最多 32，hook 只留最新）
    end
    alt stop、close 或事件循环析构
        Port->>App: 挂起的 next 结果为 aborted
    end
    Note over W: 成功的下一次 start 开启新纪元，旧 next 租约不跟过去
```

## 9. Work 晚到取消仍返回计算结果

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant Q as queueWork
    participant Pool as 线程池
    participant EvLoop as Loop 线程

    App->>Q: start，uv_queue_work
    Pool->>Pool: 执行 fn（可轮询 token）
    Note over App: 此时 request_stop
    App->>Q: cancel：uv_cancel 失败因为已在跑，于是 request_stop 给 fn
    Pool->>Pool: fn 算完，结果写入存储
    Pool-->>EvLoop: after-work 回调
    EvLoop->>Q: complete，即使已经 stop
    Q->>App: finish 返回计算结果，而不是 canceled
    Note over Q: 若取消发生在入队且 uv_cancel 成功，fn 不运行，结果才是 canceled
```

## 10. KCP：发送、收包、update 与 check 定时器

```mermaid
sequenceDiagram
    participant App as 用户协程
    participant Sess as KcpSession
    participant Core as KcpCore
    participant Out as 有界输出队列
    participant Udp as Udp
    participant T as ikcp_check 定时器
    participant Peer as 对端 UDP

    App->>Sess: sendMessage(msg)
    Sess->>Sess: 背压（条数与字节水位）
    Sess->>Core: send（传入外部时钟）
    Core->>Out: output 回调只拷贝，不发 I/O
    Sess->>Udp: 发送泵异步 send 拷贝后的包
    Udp->>Peer: UDP 数据报
    Peer-->>Udp: 入站数据报
    Udp->>Sess: receive 泵 route
    Sess->>Core: input
    opt 默认立即 flush
        Core->>Out: 尽快发出 ACK
    end
    Core-->>T: ikcp_check 得到下次绝对时间
    T-->>Sess: 到期后 update，可能重传
    Core-->>Sess: 重组完成
    Sess-->>App: recvMessage 交付一条消息
    Note over Core: 核心不读时钟、不碰 socket；丢包后即使没有收包事件也会靠定时器重传
```

## 11. acceptPeer 接入未知 conv

```mermaid
sequenceDiagram
    participant Peer as 远端
    participant Ep as KcpEndpoint
    participant Cand as 候选表
    participant App as 用户协程
    participant Sess as KcpSession

    Note over Ep: 创建时 KcpEndpointOptions.accept 为 true
    Peer->>Ep: 未知 conv 的首个合法数据报
    Ep->>Cand: 速率与 per-peer 限制内收下，带 TTL
    App->>Ep: CO2_AWAIT acceptPeer()
    Ep-->>App: KcpIncoming（peer、conv、首包）
    App->>Ep: openSession(incoming, options)
    Ep->>Sess: 创建会话并把首包 input 进核心
    App->>Sess: sendMessage / recvMessage
    Note over Cand: incoming 只能用一次；过期或属于别的 endpoint 会被拒绝
```
