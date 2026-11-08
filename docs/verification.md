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

## KCP 后续实现验证

2026-11-08（Asia/Shanghai），代码至a7c7131；Linux x86_64，g++13.3、clang++18.1。

| 构建 | 结果 |
|---|---|
| g++ Debug | 72/72；每项连续重复3次 |
| clang++ Debug | 72/72；每项连续重复3次 |
| ASan+UBSan / leak | 72/72；每项连续重复3次；C/C++均完整instrument |
| TSan | 72/72；每项连续重复3次；C/C++均完整instrument |

无sanitizer suppression，无禁用vendor检查；原始c/h/LICENSE的固定SHA256通过。
新增实际RED→GREEN包括offsetof UB、超限消息queue-head、重复显式flush、字节水位取消、
stream接收limit混用、compiled包架构误标，以及native close内联恢复、16位接收窗溢出和stream终止错误优先级。保护器、分位数与此前模块创建也先确认缺失RED。
已正确的寿命/accept路径新增覆盖后直接通过，没有把这些用例虚称为原有失败。

覆盖 pending接收close、busy/done未take、真实saved awaiter、Loop-first内部pump与在途send、
提交后结果冻结、发送前取消/字节背压、共享conv隔离、重复fd计数、候选TTL/错误endpoint/重放/取消/关闭/限额。
Linux寿命fixture强制UDP走native异步请求，避免try_send成功掩盖在途路径；其他平台未宣称已运行该fixture。

KCP installed/relocated、add_subdirectory、FetchContent消费者各为两个TU，随四构建重复；
另验证OFF安装包拒绝required kcp component。CI actionlint通过；Linux所有矩阵启用KCP并传递C/C++ sanitizer flags。
GitHub远端CI本轮未运行（未push），不能把本地结果说成GitHub Actions已成功。

Release基准已运行，结果与局限见 [benchmark-kcp.md](benchmark-kcp.md)。
部分vendor OOM不可恢复、串行TX以及未验证平台仍是已知限制，不等同于测试失败。
