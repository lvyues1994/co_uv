# 验证记录与平台范围

这里记录可复现的验证范围，不引用某台机器的临时日志路径。历史条目对应当时源码，不能替代当前结果。

## 本轮独立 review 修复

本轮最终源码在四配置各通过 **63 个 CTest targets ×3**，再各通过 **16 个受影响目标 ×20**。
逐个 target 核对重复次数，全部成功，无 sanitizer 报告。包括公共头、安装/搬迁、独立消费者、两 TU 与 installed MIT notice。
ASan 启用 leak detection；没有修改本地系统配置。
独立 review 的两个代码 repro 已转为 blocked-write 与 dual-family DNS 回归，先验证 RED 再修复。
额外覆盖 native/OOM 构造失败、部分写入失败、无分配 local/remote schedule 与 late Work cancellation。
`actionlint 1.7.12` 检查通过；clang-format 18 的四文件 dry-run 和 token-equivalence 检查通过。
格式提交仅格式，不含逻辑修改；最后提交仅文档。
这些是本地结果，未 push，因此未触发或声称远程 GitHub CI 已绿。
使用 GCC、Clang、Clang ASan+UBSan、Clang TSan；CTest 统一 output-on-failure。
覆盖 blocked write timeout/直接 stop、部分写入提交失败、双族 DNS 故障、Work queued/running/late cancel、
Result factories 的 native/OOM 注入、local/remote scheduler 无分配、ExitStatus 调用者与安装 license。
Linux fault injection 使用 linker wrap，保留 sanitizer allocator interceptor；子进程 fixture 为原生 helper。

## 历史 checkpoint

| 阶段 | CTest targets | GCC / Clang 重复 | ASan+UBSan / TSan 重复 |
|---|---:|---:|---:|
| 生命周期 review | 21 | 5 / 5 | 10 / 20 |
| whenAny throwing result | 22 | 5 / 5 | 10 / 20 |
| Result-aware all、DNS、Work | 27 | 5 / 5 | 10 / 20 |
| P2 capture/buffer/Dir/yield | 38 | 5 / 5 | 10 / 20 |
| install/export | 40 | 2 / 2 | 2 / 2 |
| FsEvent/FsPoll | 45 | 3 / 3 | 3 / 3 |
| Block 2 watcher/Signal/phase/TTY | 54 | 3 / 3 | 3 / 3 |
| DNS cache/HE/POSIX group + 自查 | 59 | 3 / 3 | 3 / 3 |

Fs watchers 另有5个目标各20轮；Block 2 前半段另14个目标各20轮，后半段最终18个目标各20轮。
安装验证包括独立 find_package、前缀搬迁、add_subdirectory/FetchContent 与两 TU；公共头独立编译。
CI 接入历史还覆盖 GCC/Clang Debug+Release、两种 sanitizer 共六配置，以及强制依赖发现/独立安装。
CMake 3.28 的独立包消费曾验证；3.14、多配置生成器仍需实机补充。

## 复现与解释

按 README 的 presets configure、build、ctest。可用 ctest 的 `--repeat until-fail:3` 检查完整重复，
用 `-R` 选择针对性测试并 `--repeat until-fail:20`。
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1；UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1；
TSAN_OPTIONS=halt_on_error=1。日志可作为 CI artifact 保存，不依赖开发者目录。
运行 `actionlint .github/workflows/ci.yml` 检查 workflow 结构；这不等于实际远程 CI 运行成功。

## 平台与限制

本地验证平台为 Linux。macOS/Windows CI 为 continue-on-error 的实验项，QNX 尚未目标机验证。
没有修改本地 sysctl，也没有关闭 sanitizer 或过滤报告来换取通过。
真实 NSS、网络 FS、终端与信号 disposition 有系统语义差异；测试不构成硬实时保证。

## KCP 实验实现：未完成验收

新增测试均先确认RED：vendor缺失、Core缺失、runtime缺失、stream缺失、accept缺失；
endpoint会话上限另有实际断言RED→GREEN。纯核覆盖模拟时钟/可重复丢包乱序、回绕、片数与整包预检、空消息、stream、dead_link。
已跑真实UDP消息回环、stream小buffer余量与短写、取消、close后awaiter引用、有界候选/显式接入。

四构建编译成功；g++/clang++/TSan 首次全套及随后每项连续3次重复均68/68，ASan+UBSan 64/68。
ASan+UBSan四项KCP失败统一定位到原样vendor的IOFFSETOF空指针成员取址宏；未屏蔽、未修改。
因此不能声称四构建全绿或KCP已验收，不能用本记录代替尚未补齐的关闭/并发/包/基准验证。
详见[设计的新审批点](design-kcp.md#14-实现进度与新审批点)。未运行性能benchmark。
