# KCP latency smoke benchmark

2026-11-08（Asia/Shanghai），用户机器 Linux x86_64，GCC13.3 Release；libuv1.48、固定KCP V2。
测量代码5387b3d；之后827ab69/a7c7131仅增加参数上界与同步拒绝校验，不改变本基准默认数据路径。
这是功能性/低延迟路径基准，不是公网性能资格认证。无CPU绑核、频率锁定、隔离负载或置信区间；
每种配置仅一轮，排除100次预热后记录表中样本数。输入为固定长度消息、串行echo、同一Loop，
TCP_NODELAY开启；KCP默认1/20/2/0、MTU1200，分别显式切换input即时flush。

## 复现

```sh
cmake -S . -B build/kcp-bench -DCMAKE_BUILD_TYPE=Release \
  -DUVCO_ENABLE_KCP=ON -DUVCO_BUILD_KCP_BENCHMARK=ON -DUVCO_BUILD_TESTS=OFF
cmake --build build/kcp-bench --target uvco-kcp-latency
build/kcp-bench/uvco-kcp-latency --rounds 1000 --bytes 128 --sim-delay-ms 10 --sim-loss-percent 1
build/kcp-bench/uvco-kcp-latency --rounds 100 --bytes 65536 --immediate-flush 1
build/kcp-bench/uvco-kcp-latency --rounds 100 --bytes 65536 --immediate-flush 0
```

benchmark默认不构建、不进CI；错误/超时不默默剔除。分位数按nearest-rank计算，max单列。
以下均为**应用echo RTT（微秒）**，不是本地send/drain耗时。CPU为std::clock进程CPU秒，
包含预热与任务启动（TCP还含连接），不是逐消息CPU或模拟网络时间，不能当作严格公平的CPU排名。

## 本机真实IPv4 loopback

| 消息 / KCP即时flush | 协议 | 样本 | p50 µs | p99 µs | max µs | CPU s |
|---|---|---:|---:|---:|---:|---:|
| 128B / ON | KCP-native-loopback | 1000 | 15.596 | 26.613 | 28.269 | 0.019312 |
| 128B / ON | TCP-native-loopback-NODELAY | 1000 | 4.805 | 5.914 | 9.200 | 0.010519 |
| 64KiB / ON | KCP-native-loopback | 100 | 625.997 | 710.928 | 713.278 | 0.131296 |
| 64KiB / ON | TCP-native-loopback-NODELAY | 100 | 13.952 | 17.179 | 20.321 | 0.005476 |
| 64KiB / OFF | KCP-native-loopback | 100 | 80186.246 | 80790.403 | 80925.892 | 0.447751 |
| 64KiB / OFF | TCP-native-loopback-NODELAY | 100 | 67.277 | 83.661 | 98.780 | 0.021507 |

本机无损loopback上TCP明显更快，没有测得KCP“30–40%/3x”优势。
64KiB分片场景关闭即时flush会明显增加尾延迟，本轮保留默认ON的效果可见，但不外推到所有网络。
开关只改变KCP；两次64KiB TCP结果也不同，说明系统调度/频率等未受控，不能从单轮宣称统计显著性。

## 单独的KCP纯核模拟（不是TCP对照）

注入时钟1ms步进，双向各10ms延迟、每方向1%确定性伪随机丢包，seed12345/67890，
100次预热+1000次128B echo。65个模拟网络丢包；p50=20ms、p99=70ms、max=78ms，CPU=0.002183s。
模拟器没有实现匹配的TCP内核网络路径，**不得与上面的无损TCP数字计算优劣/倍数**；
这里仅证明可重复有损链路与分位数报告能运行。小延迟结果受1ms量化限制，不代表物理网卡时延。

完整性能验收仍需设计文档中的多seed、带宽/竞争流、burst/reorder、p99.9与长时间观察，
以及在另行批准的隔离网络环境中给TCP/KCP施加相同真实网络条件。本轮没有改主机网络策略。
