# uvco 数据路径与 libuv 性能架构 review

日期：2026-11-08（Asia/Shanghai）。§1–5 保留初次审计（基线1fb3851/f21802a）的证据和数字；
**已批准的前三项优化现已实施，最新状态见§6；公开语义未改变。**

## 1. 结论：不等于“端到端零拷贝”，也不等于“零开销”

- **Tcp/Pipe/TTY 原始 read/write、UDP send/recv、fs read/write：通常没有 uvco 新增的用户态 payload 中转复制**。
  read 已经支持调用者 buffer，write 已经借用调用者 buffer，并已有 scatter/gather；不用为了“零拷贝”再造一层buffer池。
- 普通socket/file系统调用仍有用户态↔内核复制；这是普通 libuv 路径共有的成本，不能称全链路zero-copy。
  `uv_buf_t`只是地址/长度描述符，不是所有权、不是内核零拷贝承诺。
  `uv_fs_sendfile`（`fs/fs.hpp:173-182`）可走平台优化文件传输路径，但不是所有API/平台的保证。
- KCP、BufferedReader、process capture、DNS拥有明确复制；`std::move(vector/string)`通常只转移所有权，不能把这些都算payload复制。
- uvco增加状态/生命周期/取消/恢复开销。**最具体的现存浪费：WriteAwaiter 200B超过co2 64B内联槽，
  即使 uv_try_write 一次成功，常规右值 CO2_AWAIT 仍有1次装箱分配/写。**“native Req快路径无分配”不等于整个await无分配。
- 本机Release五轮配对测量：128B echo p50增加3.80–4.82%；64KiB echo p50增加1.49–2.35%。
  这是单机loopback的具体数值，不是所有负载固定损耗；p99噪声大，不能宣称协程改善尾延迟。

以下仓内路径均相对于`include/uvco/`。§1–5 审计时第三方 co2 为
`a265e5771b0e3ca68d6d473b9bb410fadddf4c23`；当前 FetchContent 锁定为
`0be5ab2f1b72be3755d8d13fb02d4bf32f652539`（`UVCO_CO2_GIT_TAG`），默认 await 槽仍为 64B。

## 2. 每条数据路径的复制/所有权

分类：K=普通OS/libuv路径共有；S=当前安全/所有权契约需要；O=可经新设计避免/减少。

| 路径与证据 file:line | payload与元数据 | 分类及边界 |
|---|---|---|
| Tcp/Pipe/TTY共享stream read，`stream/stream.hpp:185-208,286-300` | alloc_cb直接返回readBuf_，内核读进用户buffer；无临时块/memcpy；每次读到数据read_stop | payload仅K；用户buffer持续有效到完成；单reader，按需read_start/stop有额外调用 |
| stream write，`stream/stream.hpp:383-447,450-461` | 借用payload；<=8个uv_buf_t栈描述符，>8个vector；先try_write，剩余prefix指针偏移后uv_write，无payload拼接 | K；借用是S；调用者buffer到完成/取消清理后才能复用；取消关闭流且等callback。部分提交后失败关闭，Result<void>不暴露已写prefix |
| writev，`stream/stream.hpp:329,387-405,551` | vector<ConstBuffer>按值/移动；调用者传左值vector会复制描述符；>8还有临时uv_buf_t数组分配；libuv也可能复制描述符 | 描述符是O，不是payload；安全borrowed span可省描述符所有权开销，但不能提前释放payload |
| UDP recv，`udp/udp.hpp:150-177,194-206` | alloc_cb使用用户recvBuf_，没有payload memcpy；SocketAddress按值复制，Result含size/sender/truncated | payload K；地址元数据S；恢复与stop延后，见§3 |
| UDP send，`udp/udp.hpp:260-268,323-361` | 借用payload；复制目标地址到awaiter；try_send成功无native Req；fallback new Req+uv_udp_send | K+地址S；send不可中途取消；等待callback前不得释放payload |
| fs read/write，`fs/fs.hpp:110-135,189-255` | uv_buf_t直接指向调用者buffer；uv_fs_t嵌入FsOp，不复制payload | K；worker/libuv内部队列/缓冲是共有成本；取消EBUSY仍等callback再释放buffer |
| readFile/writeFile，`fs/fs.hpp:568-611` | read直接进string尾部；resize扩容可搬移旧内容；writeFile按值string，左值入参复制，右值可转移；结果move | 拥有完整文件S；扩容与额外复制部分O；不是每个fs read都先读临时buffer再copy |
| BufferedReader，`stream/buffered_reader.hpp:65-75` | 4KiB帧局部bytes→data_.append→返回新string；erase消费前缀可能搬移剩余内容；每次fill另有Task帧 | 接口提供拥有型string是S；临时中转、压缩与小Task为O |
| Process stdio，`process/process.hpp:231-271` | stdio直接映射Pipe原生stream，无专属payload复制；spawn组装argv/env指针表、stdio vector，选项拥有string | stream同上；启动元数据S，不应摊成每字节复制 |
| capture/communicate，`process/capture.hpp:49-66` | stdin借用拥有的input；stdout/stderr读4KiB局部块后append到string，扩容可搬移；最终move返回 | 聚合拥有型结果S；直接读string尾部可减一次复制O；输出cap不能移除 |
| DNS，`dns/dns.hpp:18-28,37-55,78-85` | host/service按值保活；addrinfo链表复制成SocketAddress vector后free；getnameinfo复制host/service到string | S：libuv内存不能逃出callback；vector多次扩容O；不是网络payload流 |
| DNS cache，`dns/resolver.hpp:19,177,226,282-288` | miss创建flight/Task；vector move进shared_ptr；命中共享不可变列表而非复制整列表；key仍有string/map开销 | S：订阅生命周期；std::function是可注入查询/clock接口的类型擦除，不在TCP逐包路径；shared_ptr原子ref成本 |
| Work，`work/work.hpp:10-31,44-82` | callable按值保存，左值可复制闭包；worker结果原地构造，之后move到Result；异常exception_ptr跨线程转移 | 不是隐式数据memcpy；成本取决于R/闭包的move。原生线程池调度/同步K；cooperative stop状态S，见§3 |

### KCP单独看：明确不是零拷贝

- 应用`sendMessage(KcpMessage)`按值：左值先复制vector（O）；右值转移；之后
  `kcp/runtime.hpp:495-501,675-677,1070`把消息交给core。
- `ikcp_send`为分段分配并复制payload：`third_party/kcp/ikcp.c:529-535`；stream合并末段还可复制旧段
  （491-499）。**固定未修改vendor下是S，不是co2额外拷贝**。
- flush把segment编码/复制到vendor借用output buffer（ikcp.c:1177）；callback把借用buffer复制进预分配
  有界packet槽（`kcp/runtime.hpp:790-796`，S），之后UDP借用该槽发出；不再为每包另造vector。
  callback不能syscall/挂起是既定设计；不能简单删copy，vendor会覆写buffer。
- UDP读进endpoint receiveBuffer；`ikcp_input`为新段分配并复制（ikcp.c:865-876）；
  `peekMessageSize`调用peeksize，只扫队列不复制（`kcp/core.hpp:171`）；recv先resize vector，
  `ikcp_recv`重组复制到该vector（`kcp/core.hpp:243-261`、ikcp.c:396）。这是vendor拥有段的S。
- KcpStream再持有完整unit作为leftover，memcpy部分到调用者buffer
  （`kcp/runtime.hpp:637-644`）；write先copy用户bytes到拥有vector（1036附近）。
  leftover二次copy属于O，但在不改vendor的情况下仍需至少一次ikcp_recv重组；不能声称消除所有copy。
- unknown conv复制候选datagram到有界候选槽（runtime.hpp:277），是显式accept前保留数据的S。
  packet/candidate数组预分配节省逐包heap，换取endpoint常驻内存；重传仍有vendor缓冲/编码成本。

## 3. 每次await的固定成本：不是“一await一协程帧”

| 对象/路径 | 分配、同步、调度 |
|---|---|
| Task | 每次创建Task通常1个协程frame分配；同一Task里循环await不会重新分配frame。本机empty Task创建实测1 new/task。co2 `detail/frame.hpp:250-278,334-356`；Task continuation对称转移而非必进loop（`co2/task.hpp:13-20,40-47`） |
| co2 AwaitSlot | 默认64B（`co2/config.hpp:10`）；过大/不适合内联的右值awaiter在`co2/detail/await_slot.hpp:99-106` new。实测read=24B、write=200B、Sleep=360B、UDP send=64B；不要把源码旧“write<=8指针”注释当事实 |
| HandleBlock/HandleRef | 每个Tcp/Udp/Timer等拥有heap控制块，而非每次read新建。HandleRef普通计数++/--，**不是shared_ptr、不是原子ref**（`core/handle.hpp:120-133,179-185`）；句柄操作限loop线程。虚析构/close/abort及newPeer是少量虚调用，不是每字节virtual |
| WaitSlot | 控制块内联state/waiter/Result存储（`core/wait_slot.hpp:15-65`），没有隐含heap；Result值移动可能触发用户T自定义成本 |
| ReqAwaiter | req_直接嵌入awaiter，complete直接resume（`core/req_awaiter.hpp`）；左值由 co2 就地等待，帧里只记指针，适合帧局部复用。FsOp/Sleep/Work/DNS大右值会装箱，不是uv_req_t必定另外new |
| stream write | 即使try_write完成，**右值awaiter装箱1次**；需排队时还new Req（stream.hpp:431）。read常规内联无独立分配；多buffer另算描述符vector |
| UDP | send64B适合默认槽，try_send快路径无额外装箱/native Req；fallback1 native Req。recv waiter本身小且无payload分配；但接收完成schedule+stopFrame是额外调度（udp.hpp:194-206） |
| CancelHook | stop不可能时立即return；否则placement-new回调到内联storage，不为hook另new（`core/cancel.hpp:43-58,83`）。co2 StopState拥有atomic ref/request标志+mutex，注册/注销锁链表（`co2/stop_token.hpp:58-62,94-121`）；复制有效token也有atomic ref，不要把它与HandleRef混淆。停止请求post到loop再处理，含mutex+uv_async_send；disarm还unpost |
| Work | callable为模板F，非std::function；uv_work_t嵌入；source_默认构造有StopState分配，start又重建source_（work.hpp:49,81），即首次构造与首次start各分配一次，复用每次也分配stop状态。结果发布靠after-work同步，非每字节atomic；异常才走捕获/重抛 |
| whenAll/whenAllResults/whenAny | 子Task各自有frame；非Task分支包装为Task（sync/when_any.hpp:210-245）。组合器有stop_source堆状态和atomic决策/计数（368-371）；tuple槽在对象内，range有tasks vector+Entry数组（531,591-598），allResults range多个vector（when_all_results.hpp:143）。大组合器右值本身还可能装箱。结果移动到slot再胜者输出/tuple聚合；有些T的move可昂贵/抛异常 |
| timeout | whenAny(op,sleep)组合（`sync/timeout.hpp:45-76`），增加Task/停止状态/awaiter存储、定时器init/close和loser取消清理；不能当成零成本标量检查。即使已决胜，已启动分支仍drain；不能为性能跳过清理或loser结果收集 |
| Sleep vs Timer | Sleep每次init/start/close，close后才恢复，增加close phase处理（`timer/sleep.hpp:24-27,45-83`）；右值另装箱1次。帧局部Sleep不装箱但仍init/close。Timer复用控制块/原生timer，tick小awaiter，无逐tick heap |
| schedule队列 | `core/loop.hpp:108-115`构造时预留4个vector，各65536项；64位机器标称约2MiB元素容量，非每await支出，亦非都等于已提交RSS。schedule loop线程push无锁无分配，激活idle；异线程mutex+async_send（234-259）。batch swap期间新投递要下一次idle（367-373）。函数指针step/indirect resume成本存在，但没有每次std::function分配 |

**不能统一说“每个I/O多一轮loop”。** stream read/write/accept、UDP send、通用request原生callback直接resume；
UDP recv和KCP用户完成走schedule；用户close/stop栈常用deferred保证不重入。
Loop的vtable源于实现Scheduler，调用具体类型可能被编译器去虚拟化；没有理由为所有await各加一笔固定virtual费用。

## 4. 测量协议与结果

- Linux x86_64同机loopback，g++13.3 Release，系统libuv1.48；没有绑核、频率锁定、隔离主机负载。
- 两端同进程同loop；raw为libuv C API直接callback（用C++组织fixture，不用co2）；双方TCP_NODELAY、
  相同预分配payload/echo buffer、先try_write再uv_write、读够固定长度才echo、验证响应内容；不含建连/close。
- warmup100轮；128B测10000轮；64KiB测2000轮；timer/sleep测20000轮、0ms一次性重启。
- 五轮，TCP raw/uvco前后顺序交替；**计时pass关闭operator-new计数**，另五轮开计数，避免只给uvco增加atomic计数偏差。
  operator new替换仍在二进制中，两方共用malloc/free包装；不能把结果推广为默认allocator或真实服务吞吐。
- 表中为五次各自分位数的中位数，不是合并所有样本分位数；echo吞吐为单方向payload bytes/总时间，
  **64KiB stop-and-wait echo goodput，不是流水线饱和单向/网卡带宽**。timer的MiB/s字段无意义，不纳入表。

| 路径 | payload | p50 μs | p99 μs | echo MiB/s |
|---|---:|---:|---:|---:|
| raw_tcp | 128 | 4.563 | 9.974 | 24.37 |
| uvco_tcp | 128 | 4.772 | 6.698 | 25.20 |
| raw_tcp | 65536 | 13.754 | 16.229 | 4507.89 |
| uvco_tcp | 65536 | 14.028 | 16.433 | 4429.16 |
| raw_timer_reuse | 1 | 0.111 | 0.127 | — |
| raw_timer_close | 1 | 0.114 | 0.130 | — |
| uvco_sleep_rvalue | 1 | 0.143 | 0.161 | — |
| uvco_sleep_local | 1 | 0.128 | 0.146 | — |
| uvco_timer_tick | 1 | 0.129 | 0.144 | — |

128B配对p50开销3.80–4.82%，64KiB为1.49–2.35%；64KiB echo goodput中位数约低1.75%。
raw 128B五轮p99范围4.577–10.238μs，uvco为5.419–10.881μs，因此不从表中较低的uvco p99推导尾延迟优势。
TCP uvco端使用whenAll自带取消状态，raw不提供取消/安全控制块，测得的是完整封装而不是只隔离co2状态机。
root timer任务无可停止token，不能据此估算timeout的取消注册开销。
0ms timer测的是最快回调/恢复机制，不是1ms定时精度；Sleep包含init+close，raw提供reuse和close两种基线。
uvco_timer_tick在本负载p50约多18ns；Sleep右值比匹配raw-close约多29ns，帧局部Sleep约多14ns。
uvco Timer/Sleep还调用uv_update_time（timer/timer.hpp:73、timer/sleep.hpp:52），raw基线直接start；
差值包含这项附加保证，不是纯resume指令开销。这些差值接近微基准敏感区，不能当稳定的普适纳秒常数。

开计数五轮一致：raw TCP steady-state 0 new/round，uvco TCP 2 new/round（client/server各1个右值write装箱）；
Sleep右值1 new/await，帧局部Sleep与Timer tick为0；empty Task创建1 new/task。
**不是全进程分配统计**：排除warmup/Task建帧/句柄/loop创建、payload预分配；仅C++普通new/new[]，
不统计libuv/vendor malloc、内核分配、峰值驻留或释放次数。没有压力注入，因此不能用2/round涵盖fallback路径。

复现（可选目标不加入默认构建/CTest/性能门禁）：

```sh
cmake -S . -B build/perf-release -DCMAKE_BUILD_TYPE=Release \
  -DUVCO_BUILD_PERF_BENCHMARK=ON -DUVCO_BUILD_TESTS=OFF
cmake --build build/perf-release --target uvco-libuv-overhead -j
# 分别运行五次，交替有无reverse，保留标准输出；外层超时避免损坏环境挂死
build/perf-release/uvco-libuv-overhead forward no-count
build/perf-release/uvco-libuv-overhead reverse no-count
build/perf-release/uvco-libuv-overhead forward
```

本次验证：g++ Release五轮计数+五轮非计数；clang++ Release编译/运行；ASan+UBSan运行（包括泄漏检查）通过。
benchmark提交为`2bf06c3`。没有为性能表使用sanitizer结果；本任务不宣称重新完成四套全量CTest，也未推送/运行远端CI。

## 5. 初次 review 的优化排序（前三项已获批准并实施，见§6；其余仍需批准）

| 优先级 | 可落实建议与预期影响 | 成本/风险/审批 |
|---|---|---|
| P1-1 | 缩小WriteAwaiter至co2内联槽：buffer/HandleRef/小状态内联，pending后才分配Req+CancelHook稳定状态；消除已实测每write1次装箱 | 2–3天；中风险，取消回调地址、部分写/关闭和保存awaiter生命周期必须回归；需批准，不直接扩大所有Task内联槽 |
| P1-2 | BufferedReader/capture直接读入预留string尾部；维护size/offset，不经4KiB临时buffer再append；BufferedReader用游标/环形buffer减少erase | 2–3天；中风险（取消/错误后已缓冲字节保留、size回滚、cap）；结果string仍需拥有/复制，borrowed view属于新API需单独批准 |
| P1-3 | Work仅在需要合作取消时懒建/重建StopState；先量无token callable和短work的分配 | 1–2天；中风险，必须保留每次请求独立取消代际和late-cancel结果规则；优化前需测Work专项，不能从TCP推断收益 |
| P2-1 | KcpStream的read_some在目标buffer足够容纳unit时直接ikcp_recv到用户buffer，否则保留leftover；write借用入口仅在KCP立即接纳时使用 | 2–4天；中高风险，stream单位、backpressure后借用buffer寿命要精确定义；不得修改vendor或删output copy，新增借用API需批准 |
| P2-2 | writev借用descriptor span/小固定数组重载，避免vector<ConstBuffer>与>8临时descriptor分配 | 1–2天；中风险，descriptor可即时复制到req但payload仍借用；现有writev已不拼接payload，收益取决于实际buffer数 |
| P2-3 | 高创建率Task采用co2帧allocator/pool，Req可按loop有界池化 | 3–5天；高风险（线程归属、alignment、异常、pool出寿命）；先测Task churn/RSS，需与co2 allocator扩展点协调，不在uvco复制其detail实现 |
| P2-4 | Loop容量按工作负载显式配置，缓解默认约2MiB容量预留；socket control block是否pool以accept基准决定 | 配置已有，无需改API；inline整个uv_handle_t到可移动Tcp会破坏地址稳定性，不建议盲改；池化需批准与独立证据 |
| 不建议 | 为“低延迟”把UDP recv/KCP全部改内联恢复；去掉取消hook、drain、HandleRef，或去掉KCP output队列copy | 不是等价优化；重入/资源安全/已批准语义会变化，需新设计审批。TCP当前已inline，不能重复优化不存在的hop |

**不建议优先做“新增caller read buffer”或“给所有读加buffer pool”**：前者已存在，后者可能平白引入中转copy。
不要把libuv共有的内核copy、TCP本身、KCP协议冗余算成co2包装损耗；也不要把安全契约当成免费。
后续若要给生产容量结论，应增加单向流水线TCP吞吐、真实backpressure、UDP批量与recv hop、取消开启/关闭、
Work/DNS、Task创建率与内存峰值的专项，固定CPU/多进程多次采样并报告区间。

## 6. 已批准的三项优化（2026-11-08，Asia/Shanghai）

### 实施与契约

1. `771fb18`：`stream/stream.hpp` 把 CancelHook 移到按需创建的 native write Req。
   x86_64 WriteAwaiter 从200B降到64B，默认co2槽可内联；try_write全部成功无awaiter装箱/native Req分配。
   排队路径仍分配Req，取消仍关闭整个流、等原生callback再释放借用buffer；部分写失败仍关闭。
   writev描述符容器与调用者payload所有权没有变化，不宣称所有writev/背压写都零分配。
2. `d092258`：共用内部 `detail/string_read_tail.hpp`，先resize拥有string，再直接提供尾部buffer，
   成功commit实际长度，EOF/错误/异常/取消rollback未提交部分；BufferedReader.bufferedBytes不暴露pending tail。
   省去每次4KiB frame临时区→append的payload copy，fill Task和最终拥有型返回string仍存在；消费前缀erase仍可能搬移。
   C++14 resize仍初始化尾部并可能搬移扩容，EOF前的预扩容也可能产生多余分配；这不是任意负载必胜或端到端零拷贝。
   capture每次最多扩展到剩余cap，cap耗尽用1字节probe区别EOF/越界；零cap和恰好达到cap都已回归。
   改变的是内部读取块尺寸/分配时机，不改变公开的output cap、Result错误、取消与reap规则。
3. `7943445`：Work初始nostopstate；无token callable不构造内部停止状态；token-aware callable在每次真正start时
   分配新状态，保留stop_possible、合作停止与重新提交代际隔离；双重载仍优先token版本。
   仍然有Task frame/大右值awaiter可能装箱，异常仍在after-work清理后重抛；晚到取消继续返回已完成结果。
4. `51410ca`：测试fixture用链接wrap调用真实new/new[]，保留ASan/TSan allocator interceptors。
   最初直接定义operator new与Clang TSan运行库强符号冲突，已修正测试工具；没有关闭任何库/新测试的sanitizer。

### RED→GREEN 回归证据

- write allocation：优化前`count==0`失败（右值awaiter装箱），优化后同一测试通过，另验证sizeof不超过co2默认槽；
  原有pending cancel、partial write故障测试保持通过。
- read copy：优化前BufferedReader和capture的受拦截append字节数非零，优化后都是零。
  ABI相关copy fixture只在Linux/libstdc++ C++11 ABI注册；普通行为测试继续跨平台。
  另验证bufferedBytes逻辑长度、抛异常后保留已提交4096B并恢复读取、cap=0/4096恰满、超过cap。
- Work allocation：优化前构造plain/cooperative Work各1次new，`n==0`失败；优化后构造皆0。
  使用帧外lvalue Work（排除Task/装箱），重复提交plain每次0，cooperative每次1个停止状态分配；
  原有取消、晚取消、worker异常测试保持通过。
- 计数只覆盖C++分配调用，不覆盖libuv malloc/内核或worker内任意用户分配；不会把结果泛化为整个queueWork API零分配。

### 复测

已完成新旧同窗复测：旧基线`f21802a`的include通过git archive提取到项目build下（不切换/重置工作树），
新代码`51410ca`；相同g++13.3 `-O3 -DNDEBUG`、co2、系统libuv1.48、相同benchmark源码。
在所有build/CTest结束后运行，五轮交替新/旧进程先后顺序，各进程内也交替raw/uvco TCP先后；
另各五轮开计数。保留§4相同warmup/样本数、单机loopback、无绑核/频率锁定条件。
以下仍是五次分位数的中位数，吞吐仍为stop-and-wait echo goodput。

| 场景 | p50 μs 前→后 | p99 μs 前→后 | echo MiB/s 前→后 |
|---|---:|---:|---:|
| raw_tcp / 128B | 3.981 → 3.976 | 7.923 → 9.928 | 27.600 → 25.460 |
| uvco_tcp / 128B | 4.147 → 4.112 | 4.806 → 5.528 | 29.160 → 25.750 |
| raw_tcp / 65536B | 12.479 → 12.513 | 14.737 → 14.695 | 4967.020 → 4960.860 |
| uvco_tcp / 65536B | 12.685 → 12.681 | 14.927 → 14.956 | 4882.900 → 4899.200 |
| raw_timer_reuse / 1B | 0.110 → 0.111 | 0.126 → 0.128 | — |
| raw_timer_close / 1B | 0.115 → 0.114 | 0.130 → 0.129 | — |
| uvco_sleep_rvalue / 1B | 0.143 → 0.145 | 0.160 → 0.161 | — |
| uvco_sleep_local / 1B | 0.129 → 0.130 | 0.147 → 0.147 | — |
| uvco_timer_tick / 1B | 0.130 → 0.130 | 0.145 → 0.143 | — |

- 128B TCP p50 4.147→4.112μs（约-0.84%），64KiB 12.685→12.681μs（约-0.03%）。
  相对同进程raw的配对p50开销中位数：128B 4.01%→3.61%，64KiB 1.49%→1.31%。
  不将这些小差值当作已证明的稳定加速：128B uvco goodput 29.16→25.75 MiB/s，
  raw也由27.60→25.46，且尾延迟漂移明显；保留不利数据，不用选择最佳轮次掩盖噪声。
- **分配结论稳定且可归因**：五轮旧版TCP steady-state都是2 new/echo round，新版都是0，
  即本负载每个热路径write await从1降到0；read仍0。没有把原生uv_write fallback、writev容器或建帧算入此0。
- Sleep右值仍1 new/await，帧局部Sleep、Timer tick仍0；empty Task仍1 new/task。其计时接近原值，未作这些优化。
- BufferedReader/capture的临时append复制已由独立回归验证移除；Work构造/提交分配由独立计数测试验证。
  本TCP benchmark不经过它们，不能用TCP数字声称它们的实际CPU加速。

### 最终验证

| 构建 | 完整CTest | 每项连续重复 | 实际执行 |
|---|---:|---:|---:|
| g++ Debug | 75/75 | 3 | 225 |
| clang++ Debug | 75/75 | 3 | 225 |
| ASan+UBSan（含leak检查） | 75/75 | 3 | 225 |
| TSan | 75/75 | 3 | 225 |

四套共900次执行全通过，包含新增分配/copy测试、既有取消/部分写/异常/drain、KCP、安装/源码/两TU consumer。
Linux C/C++ sanitizer flags保持开启；未推送，未运行远端CI，未额外声称macOS/Windows原生实测。
历史§4中的初次验证范围不代表本节最新结果。
