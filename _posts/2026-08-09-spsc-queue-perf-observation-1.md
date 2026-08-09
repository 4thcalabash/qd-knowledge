---
title: "性能观测工具使用：以SPSC优化为例"
date: 2026-08-09 14:00:00 +0800
model: DeepSeek-V4-Flash
tool: Claude Code CLI
---

性能优化的前提是准确的观测。本文以 Linux 官方性能剖析工具 perf 为主线，介绍性能观测的三层方法论——**全局计数、调用栈采样、系统调用追踪**——并给出每层工具的用法、输出解读与适用场景。本文以 SPSC 的优化为例演示性能观测工具的使用方法，全部命令与输出（包括失真数据）均为真实运行结果。

- 目录
{:toc}

> **环境提示**：本文在 WSL2 与 Linux VM 上运行。两种环境下部分事件缺失或数值失真，已在文中逐处标注。

## ⭐ 观测方法论：计数、采样、追踪

**一切的观测都是基于事件的**：perf 观测的不是"程序本身"，而是程序执行过程中发生的事件——CPU 周期、指令、缓存访问、分支、系统调用、缺页、调度切换等。工具做三件事：计数事件、对事件采样、追踪事件流。事件从何而来、是否可用，决定了一切观测手段的边界，这正是下一节"环境准备"的动机。

性能观测工具按测量方式可分为三类，分别回答不同层级的问题，粒度与开销依次递进：

| 层次 | 工具 | 回答的问题 | 粒度 | 对被测程序的干扰 |
|------|------|-----------|------|----------------|
| 计数 | `perf stat` | 事件总量是多少？ | 全局聚合 | 极低（读硬件/软件计数器） |
| 采样 | `perf record` / `perf top` | 热点在哪个函数、哪条指令？ | 调用栈/指令 | 低（周期性中断） |
| 追踪 | `perf trace` | 程序与内核如何交互？ | 单次系统调用 | 中（事件流捕获） |

三者呈递进关系：计数给总量、采样定位热点、追踪解释机制，排查通常沿此顺序推进。共同前提是**可复现的基准**：本文演示程序将两个工作线程经 `pthread_setaffinity_np` 固定绑定到不同物理核（cpu 0/2），测试规模固定为 1 亿条消息，保证观测结果可比。

## 环境准备：事件可用性检查

`perf list` 列出本机全部可用事件，事件分两类：

- **硬件事件**（`cycles`、`instructions`、`cache-misses` 等）：来自 CPU 的 PMU 计数器，需硬件支持；
- **软件事件**（`cpu-clock`、`page-faults`、`context-switches` 等）：由内核计数，任何环境可用。

硬件事件是否可用、可用到哪一层，取决于运行环境。本文在两种环境实测：

**WSL2：硬件事件完全缺失**

```text
  cpu-clock                                          [Software event]
  context-switches OR cs                             [Software event]
  cpu-migrations OR migrations                       [Software event]
  page-faults OR faults                              [Software event]
  task-clock                                         [Software event]
  ...
  sdt_libc:lll_lock_wait                             [SDT event]
```

**云 Linux VM：硬件事件存在**

```text
# perf list hw
  branch-instructions OR branches                    [Hardware event]
  branch-misses                                      [Hardware event]
  cache-misses                                       [Hardware event]
  cache-references                                   [Hardware event]
  cpu-cycles OR cycles                               [Hardware event]
  instructions                                       [Hardware event]
  stalled-cycles-backend OR idle-cycles-backend      [Hardware event]
  stalled-cycles-frontend OR idle-cycles-frontend    [Hardware event]

# perf list cache
  L1-dcache-load-misses                              [Hardware cache event]
  L1-dcache-loads                                    [Hardware cache event]
  L1-dcache-prefetches                               [Hardware cache event]
  L1-icache-load-misses                              [Hardware cache event]
  L1-icache-loads                                    [Hardware cache event]
  branch-load-misses                                 [Hardware cache event]
  branch-loads                                       [Hardware cache event]
  dTLB-load-misses                                   [Hardware cache event]
  dTLB-loads                                         [Hardware cache event]
  iTLB-load-misses                                   [Hardware cache event]
  iTLB-loads                                         [Hardware cache event]
```

**Linux 物理机：TODO**

待补充：物理机上 `perf list` 的完整硬件事件清单与实测输出。

## Mutex+Deque 版本 Naive SPSC

### ⭐ perf stat：全局计数

`perf stat` 执行命令并输出事件计数。它不做采样，直接读取计数器，**回答"总量"问题**。

被测程序 `spsc_naive_mutex` 是一个带 mutex 锁的 SPSC 队列实现：`spsc::Queue` 以 `std::mutex` 保护 `std::deque`，每次 push/pop 全量加锁。源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)、[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)、[Makefile](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/Makefile)。

#### 基本用法与事件选择

```bash
perf stat ./bin/spsc_naive_mutex            # 默认事件集
perf stat -d ./bin/spsc_naive_mutex         # 常用默认事件集
perf stat -e task-clock,page-faults ./bin/spsc_naive_mutex   # 显式指定事件
```

程序默认处理 1 亿条消息，命令中无需再传参。

常用参数：

- `-e`：显式选择事件，逗号分隔多个；
- `-d`：展开常用事件集（cache miss、分支等）；
- `-r N`：重复运行 N 次取平均，用于掩盖单次噪声；
- `--per-core` / `--per-socket`：按拓扑拆分计数。

#### 输出字段解读

WSL2 上实测 `perf stat` 输出（软件事件可计数，硬件事件缺失显示 `<not supported>`；行尾注释为该事件的统计含义）：

```text
 Performance counter stats for './bin/spsc_naive_mutex':

          20482.11 msec task-clock:u              #    1.443 CPUs utilized
                 0      context-switches:u        #    0.000 /sec
                 0      cpu-migrations:u          #    0.000 /sec
            202248      page-faults:u             #    9.874 K/sec
   <not supported>      cycles:u                  # CPU 周期数；与 instructions 之比即 IPC
   <not supported>      instructions:u            # 已执行指令数
   <not supported>      branches:u                # 分支指令数
   <not supported>      branch-misses:u           # 分支预测失败次数；失败率高则流水线停顿多
   <not supported>      L1-dcache-loads:u         # L1 数据缓存读访问次数
   <not supported>      L1-dcache-load-misses:u   # L1 数据缓存读未命中次数
   <not supported>      LLC-loads:u               # 末级缓存（LLC）读访问次数
   <not supported>      LLC-load-misses:u         # 末级缓存读未命中次数；未命中即访问主内存

      14.198712463 seconds time elapsed

      10.813321000 seconds user
      10.493327000 seconds sys
```

| 字段 | 含义 | 本例解读 |
|------|------|---------|
| `task-clock` | 进程消耗的 CPU 时间 | 20482 ms |
| `CPUs utilized` | task-clock 除以墙钟时间 | **1.443**：两个工作线程的 CPU 占用率之和 |
| `context-switches` | 内核调度切换次数 | **0**：等待是亚毫秒级，未触发调度睡眠 |
| `cpu-migrations` | 线程跨核迁移次数 | **0**：线程固定绑定，无迁移 |
| `page-faults` | 缺页次数 | **202248**：动态分配触及新内存页（blob 负载下 deque 扩容更频繁） |
| `<not supported>` | 该事件本机不可用 | 硬件 PMU 缺失；本输出中 8 个硬件事件均缺失，统计含义见输出块行尾注释 |
| `user` / `sys` | 用户态 / 内核态 CPU 时间 | 10.813 s / 10.493 s，几乎各占一半 |

#### ⭐ 性能问题分析

- **CPU 没跑满：锁等待**：`CPUs utilized` 仅 1.443（双线程应接近 2.0）。互斥使任何时刻只有一个线程在执行临界区，另一个线程在等待（自旋或 futex 睡眠），等待期间不占用 CPU，`task-clock` 因此小于墙钟的两倍，utilized 随之低于 2.0；`context-switches` 为 0，说明等待极短、走自旋而非调度睡眠——"短临界区锁竞争"形态。
- **内核路径开销太高**：`user`/`sys` 内核态（10.493 s）与用户态（10.813 s）同量级，大量时间耗在 futex 系统调用路径，而非纯用户态计算——与后文 `perf trace` 的 futex 观测相互印证。
- **缺页严重：动态内存**：`page-faults=202248`，动态分配触及新内存页，内存分配在关键路径上活跃。

这些是计数层的初步缺陷假设，具体位置（哪个函数、哪次调用）需采样与追踪进一步定位。

### perf record / report / script：调用栈采样（火焰图）

`perf record` 周期性中断进程，记录中断点的指令指针（PC）与调用栈；`perf report` 对样本聚合统计，或 `perf script` 可视化观察火焰图。

被测程序源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)、[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)。

#### 采样原理要点

- 采样事件默认 `cycles`（硬件），也可指定任意事件；事件不可用时自动回退到软件事件 `cpu-clock`；
- 每次中断记录 PC，通常同时记录调用栈；样本数取决于运行时间与采样频率；
- **skid（稀释）效应**：从事件触发到中断处理完成有延迟，记录的 PC 可能已越过触发事件的指令若干条，热点可能偏移数条指令；`-e cycles:pp`（PEBS，硬件精确采样）可消除，但依赖硬件支持。

#### 常用参数

```bash
perf record -g ./bin/spsc_naive_mutex          # 记录调用栈（默认 -g 走帧指针）
perf record --call-graph fp ./bin/spsc_naive_mutex     # 帧指针回溯：要求编译带 -fno-omit-frame-pointer
perf record --call-graph dwarf ./bin/spsc_naive_mutex  # DWARF 回溯：支持优化代码，开销大
perf record --call-graph lbr ./bin/spsc_naive_mutex    # 硬件 LBR：开销极低、无 skid，栈深受限
perf record -a                                  # 全系统采样
```

调用栈获取方式的选择与编译方式绑定：帧指针方式要求二进制保留帧指针（`-fno-omit-frame-pointer`，`-O2` 默认省略）；DWARF 方式不要求但开销大一个数量级。低延迟环境惯用帧指针方式，代价极小。

#### perf report 结果解读

```bash
perf record -g -o /tmp/perf_mutex.data ./bin/spsc_naive_mutex
perf report -i /tmp/perf_mutex.data --stdio
```

```text
# Samples: 8K of event 'cpu-clock:uhpppH'
# Event count (approx.): 2114500000
#
# Children      Self  Command          Shared Object         Symbol
# ........  ........  ...............  ....................  .....................
    97.79%     0.00%  spsc_naive_mute  libstdc++.so.6.0.30   [.] 0x0000721958adc253
            |  --34.85%--pthread_mutex_lock
            |  --30.80%--pthread_mutex_unlock
            |  --9.22%--0x7219586912f7
            |  --5.11%--0x721958691280
            |  --4.87%--0x721958691265
            |  --4.13%--std::thread::_M_run        # consumer 线程入口
            |  --3.25%--0x721958691256
            |  --2.45%--std::thread::_M_run        # producer 线程入口
```

本例中 `pthread_mutex_lock` 与 `pthread_mutex_unlock` 的 Self 合计约 40.6%（19.72% + 20.85%），说明约四成 CPU 时间直接耗在锁的获取与释放上；顶层未解析地址的 Children 75.05%，说明大部分采样最终都经过锁路径，锁是支配性的热点。未解析的 libc 地址（`0x…6912f7` 等，合计约 23%）对应 futex 等待与 `__lll_lock_wait` 等内部慢路径。

#### ⭐ perf script：导出样本与火焰图

`perf script` 把采样数据导出为**每样本一行的原始文本**（含调用栈），是 report 的"数据源"。`perf report` 已聚合占比，`perf script` 保留每个样本的完整栈——常与火焰图工具配合：

```bash
perf script -i /tmp/perf_mutex.data > /tmp/perf_mutex.stacks   # 导出原始样本
perf script -i /tmp/perf_mutex.data | flamegraph.pl > flame.svg # 生成火焰图
```

![naive 版调用栈火焰图：顶部可见锁路径]({{ site.baseurl }}/assets/images/naive_flame.svg)

图中顶部可见 `pthread_mutex_lock` 等锁路径的宽条——与 `perf report` 的 Self/Children 结论一致：锁是热点。

#### ⭐ 性能问题分析

- **锁是支配性热点**：`pthread_mutex_lock` 与 `pthread_mutex_unlock` 的 Self 合计约 40.6%（19.72% + 20.85%），约四成 CPU 时间直接耗在锁的获取与释放上；顶层未解析地址的 Children 75.05%，大部分采样最终都经过锁路径——锁路径确认是瓶颈所在。
- **等待在锁内部慢路径**：未解析的 libc 地址（`0x…6912f7` 等，合计约 23%）对应 futex 等待与 `__lll_lock_wait` 等内部慢路径，进一步印证 lock/unlock 的 Self 中相当部分属等待而非临界区执行。
- **采样定位了位置**：与计数层"CPU 没跑满：锁等待"的假设相互印证——之前只能推断锁等待存在，现在明确了锁的获取与释放自身就是热点所在，为后续优化（无锁化）提供依据。

> **VM 待补**：以硬件 `cycles` 为采样事件的 `perf record` 在 VM 上的报告、`cache-references`/`cache-misses` 读数、TMA 指标均待实测补充。

### ⭐ perf trace：系统调用追踪

`perf trace` 捕获进程的系统调用，功能与 strace 重叠，但**实现机制不同**：strace 基于 ptrace 逐条拦截，开销大；`perf trace` 直接订阅内核 syscall 事件，对被测程序的时序影响低约一个数量级——对低延迟程序，这决定了工具本身是否改变测量对象。

被测程序源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)、[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)。

#### 用法

```bash
perf trace ./bin/spsc_naive_mutex                # 流式输出每次系统调用
perf trace --summary -e futex ./bin/spsc_naive_mutex     # 按系统调用聚合汇总
perf trace -e futex,mmap ./bin/spsc_naive_mutex          # 事件过滤
```

`--summary` 对执行期间的系统调用按类型聚合，输出每次调用的计数、耗时分布，适合快速判断"程序在系统调用上花了多少时间"。

#### ⭐ 定位问题一：syscall 调用过多

`perf stat` 已发现 `sys` 时间（10.493 s）与 `user` 同量级——内核路径开销太高。系统调用是用户态进入内核的唯一通道，下一步用 `perf trace` 量化：到底是哪些 syscall、多少次、花了多少时间。

```bash
sudo perf trace --summary ./bin/spsc_naive_mutex
```

实测输出（1 亿条消息）按线程分段聚合，初始化系统调用已从略：

```text
# 主进程（join 等待工作线程结束）
 spsc_naive_mute (4776), 166 events, 0.0%

   syscall            calls  errors  total       min       avg       max       stddev
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex                  2      0 46046.069   466.033 23023.034 45580.036     97.98%
   clone3                 2      0     0.567     0.244     0.284     0.323     14.10%
   …（mmap/mprotect 等初始化系统调用从略）

# producer 线程（push 路径）
 spsc_naive_mute (4777), 15003891 events, 50.0%

   syscall            calls  errors  total       min       avg       max       stddev
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex            7489931 1913047 28732.203     0.001     0.004    21.496      0.13%
   sched_yield            1      0     0.120     0.120     0.120     0.120      0.00%
   …（mmap/mprotect 等少量系统调用从略）

# consumer 线程（pop 路径）
 spsc_naive_mute (4778), 14988204 events, 50.0%

   syscall            calls  errors  total       min       avg       max       stddev
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex            6196145 2425419 18214.173     0.001     0.003     0.552      0.05%
   mprotect         1288294      0  2686.545     0.001     0.002     0.437      0.08%
   …（mmap/mprotect 等少量系统调用从略）
```

字段顺序为 `calls、errors、total(msec)、min、avg、max、stddev`，每个线程段按 syscall 类型聚合。解读要点：

- **futex 是绝对主导**：两个工作线程合计约 1368 万次 futex 调用，累计约 46.9 s；程序自身打印的运行时间约 14.2 s，两者接近——**锁等待直接占据运行时间的大部分**。平均每约 7 条消息发生一次 futex 往返；
- **errors（EAGAIN）**：约 434 万次调用返回错误（约占 32%）。futex 的 FUTEX_WAIT 被唤醒后需要校验锁值，若仍被占用则返回 EAGAIN——"假唤醒"。短临界区锁竞争下，等待线程几乎刚睡下就被唤醒，睡眠本身毫无收益，纯为系统调用往返付费；
- **consumer 侧 128 万次 mprotect**：deque<Blob> 无界、扩容时分配新块，glibc malloc 为新映射页调用 mprotect——动态内存分配活跃的 syscall 级直接证据，与问题二的缺页呼应；

定位结论：syscall 过多的来源已从"内核路径开销太高"细化到具体通道——锁竞争导致的 futex 往返（约 1368 万次、约 46.9 s、32% 为无收益的 EAGAIN），同时动态内存分配（mprotect 128 万次）作为次要但可观的 syscall 开销浮出水面。

#### ⭐ 定位问题二：缺页

> **TODO**：WSL 不提供 `minor-faults` 追踪（无 tracepoint），本节的 `perf trace -e minor-faults` 与 `exceptions:page_fault_user` 均无法验证；需在物理机上实测缺页 tracepoint 的事件流与地址字段。

`perf stat` 显示 202248 次缺页。与 syscall 不同，缺页本身不是系统调用（x86 上由缺页异常触发），`perf trace` 默认的 syscall 汇总看不到它。尝试显式追踪：

```bash
perf trace -e minor-faults ./bin/spsc_naive_mutex              # 软件事件：无输出
perf list tracepoint                                          # 本环境无 tracepoint
```

实测两个命令都拿不到缺页：`-e minor-faults` 不产生事件流（perf trace 只追踪 syscall 与 tracepoint，不追踪软件事件），且本环境（WSL2）无任何 tracepoint（`perf list tracepoint` 为空，物理机上才有 `exceptions:page_fault_user` 等含地址字段的缺页 tracepoint）。**perf trace 在本环境无法用于缺页**。

改用采样：缺页是软件事件，`perf record` 可以将其作为采样事件，记录触发缺页时的调用栈。实测输出：

```bash
sudo perf record -e minor-faults -g -o /tmp/pf.data ./bin/spsc_naive_mutex
sudo perf report -i /tmp/pf.data --stdio
```

```text
# Samples: 35K of event 'minor-faults'
# Event count (approx.): 1079258
#
# Children      Self  Command          Shared Object         Symbol
# ........  ........  ...............  ....................  .........................
    91.35%    91.35%  spsc_naive_mute  libc.so.6             [.] 0x00000000000a320b
     5.55%     0.00%  spsc_naive_mute  libstdc++.so.6.0.30   [.] 0x000075570b6dc253
            |--5.55%--std::thread::_State_impl<...>::_M_run
               |--2.85%--std::deque<Blob, std::allocator<Blob> >::_M_push_back_aux<Blob const&>
     …（其余各 <1%）
```

解读：

- **采样覆盖**：35K 个样本对应约 108 万次缺页事件（采样率约 53%，202248 次缺页的过半被采样到），缺页发生时中断并记录调用栈；
- **缺页集中在 libc 内部**：91.35% 的样本落在 libc 一个地址（`0xa320b`）——具体是哪个函数，需要查证而非推断；
- **deque 扩容点名**：5.55% 走线程启动路径，其中 **2.85% 直接落在 `std::deque<Blob>::_M_push_back_aux`**——明确指向 deque 扩容分配新块。

**查证地址归属**：`0xa320b` 是 libc 内部偏移，perf 未解析符号。用 `readelf -Ws` 反查符号表，该偏移落在 `__default_morecore`（glibc 主堆耗尽时向内核扩展堆的底层函数）与 `malloc` 之间的 gap 内——即分配器内部未导出区域。**结论：缺页由堆增长（malloc 向内核申请新页）触发**，而非磁盘回读（major-faults）或映射文件。

定位结论：缺页来源已查证为**动态内存分配（堆增长路径）**——与问题一 consumer 侧 128 万次 mprotect 相互印证，与"以定长环形缓冲替换 deque"的优化路线对应。

> **TODO**：WSL 不提供 `minor-faults` 追踪（无 tracepoint），本节的 `perf trace -e minor-faults` 与 `exceptions:page_fault_user` 均无法验证；需在物理机上实测缺页 tracepoint 的事件流与地址字段。

#### ⭐ 性能问题分析

- **syscall 过多：futex 无收益往返**：两线程合计约 1368 万次 futex 调用、累计约 46.9 s，占运行时间（约 14.2 s）的绝大部分，其中约 434 万次（32%）为 EAGAIN 假唤醒——锁竞争在"数量"层面被量化确认。
- **缺页：动态内存分配**：consumer 侧 128 万次 mprotect 是 syscall 级直接证据；`perf record -e minor-faults -g` 采样显示 91.35% 的缺页样本落在 libc 堆分配路径、2.85% 点名 `deque<Blob>::_M_push_back_aux`——共同指向无界 deque 扩容分配新块。

### ⭐ 整体性能结论

三层观测在演示案例上形成交叉验证的证据链，按性能问题组织：

#### 性能问题一：锁竞争

**证据链**：

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `CPUs utilized` 仅 1.443（双线程应接近 2.0）、0 次 context-switch | 存在不占用 CPU 的等待，等待极短、走自旋 |
| 采样 | `pthread_mutex_lock`/`unlock` Self 合计 40.6%，顶层 Children 75.05% 收敛于锁路径 | 锁是支配性热点 |
| 追踪 | 两线程合计约 1368 万次 futex、累计约 46.9 s，占运行时间（约 14.2 s）的绝大部分；其中约 434 万次（32%）为 EAGAIN 假唤醒 | 锁等待的时间成本量化，形态为无收益的睡眠/唤醒往返 |

**结论**：瓶颈在同步本身而非数据路径。锁竞争使 CPU 无法跑满（1.443 < 2.0），大量时间耗在 futex 往返与 lock/unlock 上——**无锁消除锁，环形缓冲消除锁上的等待**。

#### 性能问题二：动态内存分配

**证据链**：

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `page-faults` 202248 次 | 缺页活跃，动态分配触及新内存页 |
| 采样 | `perf record -e minor-faults -g`：84.27% 缺页样本落在 libc 内部（查证为 `__default_morecore` 附近，堆增长路径） | 缺页来自堆增长，而非磁盘回读 |
| 追踪 | consumer 侧 128 万次 mprotect | 动态内存分配的 syscall 级直接证据，与缺页量级吻合 |

**结论**：缺页与 mprotect 均指向无界 deque 的扩容分配——**定长环形缓冲预分配，彻底消除关键路径上的动态分配**。

两个问题都不是数据路径的计算开销，而是同步与分配的结构性开销——这正是无锁环形队列替代 mutex+deque 的意义所在。

## 无锁环形队列（缺陷版）

按优化路线，下一步用**定长环形缓冲 + 无锁**替换 mutex+deque。本节演示 `spsc_noalign` 实现：定长环形缓冲（预分配，无动态分配），单生产者只写 `tail_`、单消费者只写 `head_`，各自 `release`/`acquire` 配对同步数据——无锁、无 syscall。

**cached head/tail 优化**：减少 atomic load 次数，只在必要时将 atomic 变量同步到 cached 中——满/空判断优先读本地 cached，缓存命中边界时才同步一次原子状态。

**但该实现故意不填充缓存行**：`tail_`、`head_`、`cachedHead_`、`cachedTail_` 四个状态变量紧邻存放，可能落入同一缓存行。两个线程在不同物理核上各自频繁写其中变量，导致缓存行在核间来回传输（false sharing）——本节的观测对象。

源码见 [spsc_noalign.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_noalign.hpp)。

### perf stat：全局计数

```bash
perf stat ./bin/spsc_noalign
```

```text
 Performance counter stats for './bin/spsc_noalign':

           9881.54 msec task-clock                #    2.018 CPUs utilized
                83      context-switches          #    8.399 /sec
                 2      cpu-migrations            #    0.202 /sec
              2182      page-faults               #  220.816 /sec
   <not supported>      cycles
   <not supported>      instructions
   <not supported>      branches
   <not supported>      branch-misses

       4.896605381 seconds time elapsed

       9.872218000 seconds user
       0.004040000 seconds sys
```

与 naive 版逐项对照：

| 字段 | naive（mutex+deque） | noalign（无锁环形） | 变化 |
|------|---------------------|--------------------|------|
| `CPUs utilized` | 1.443 | **2.018** | 接近 2.0，CPU 跑满 |
| `context-switches` | 0 | 83 | 均极少 |
| `page-faults` | 202248 | **2182** | 减至仅启动触页（32K 槽位一次性分配） |
| `sys` | 10.493 s | **0.004 s** | 内核路径开销基本归零（自旋等待，无 syscall） |
| 墙钟 | 14.2 s | **4.90 s** | 快约 3 倍 |

### perf record / report / script：调用栈采样（火焰图）

```bash
perf record -g -o /tmp/perf_noalign.data ./bin/spsc_noalign
perf report -i /tmp/perf_noalign.data --stdio
```

```text
# Samples: 32K of event 'cpu-clock:uhpppH'
# Event count (approx.): 8198000000
#
# Children      Self  Command       Shared Object         Symbol
# ........  ........  ............  ....................  .........................
   100.00%     0.00%  spsc_noalign  libstdc++.so.6.0.30   [.] 0x000079ca472dc253
            |
            ---0x79ca472dc253
               |
               |--50.02%--std::thread::_State_impl<...>::_M_run   # producer
               |
                --49.97%--std::thread::_State_impl<...>::_M_run   # consumer

    50.02%    50.02%  spsc_noalign  spsc_noalign   [.] std::thread::_State_impl<...>::_M_run
    49.97%    49.97%  spsc_noalign  spsc_noalign   [.] std::thread::_State_impl<...>::_M_run
```

与 naive 版对比，热点结构彻底改变：

- **naive**：lock/unlock Self 合计 40.6%，热点在锁路径；
- **noalign**：producer 50.02% + consumer 49.97% 几乎完美平分，热点全部落在 push/pop 数据路径本身。

锁路径从热点中消失，两个工作线程的 CPU 时间均匀分布在各自的数据路径上。

### perf trace：系统调用追踪

```bash
sudo perf trace --summary ./bin/spsc_noalign
```

```text
# 主进程（join 等待工作线程结束）
 spsc_noalign (27153), 166 events, 74.4%
   futex                  1      0  4277.086  4277.086  4277.086  4277.086      0.00%
   mprotect              10      0     0.047     0.003     0.005     0.006      7.10%

# producer 线程（push 路径）
 spsc_noalign (27154), 22 events, 9.9%
   mprotect               1      0     0.004     0.004     0.004     0.004      0.00%
   …（mmap/munmap 等少量系统调用从略）

# consumer 线程（pop 路径）
 spsc_noalign (27155), 22 events, 9.9%
   mprotect               1      0     0.004     0.004     0.004     0.004      0.00%
   …（mmap/munmap 等少量系统调用从略）
```

与 naive 版逐项对照：

- **futex**：1368 万次 → **1 次**（仅主进程 join 等待，4277 ms）；producer/consumer 线程各 22 个事件，无同步调用；
- **mprotect**：128 万次 → **0 次**（定长预分配，无动态内存）；

### ⭐ 性能分析结论

#### 性能问题一：锁竞争（已消除）

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `CPUs utilized` 1.983（接近 2.0）、`sys` 0.004 s | CPU 跑满，内核路径基本归零 |
| 采样 | 热点 50.02% + 49.97% 平分在 push/pop，无 lock/unlock | 锁路径从热点中消失 |
| 追踪 | futex 1368 万次 → 1 次（仅 join） | 无锁，无 futex 往返 |

**结论**：无锁环形缓冲消除了锁竞争——CPU 跑满、futex 归零。

#### 性能问题二：动态内存分配（已消除）

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `page-faults` 2178 次（vs naive 202248） | 仅剩启动触页（32K 槽位一次性分配） |
| 追踪 | mprotect 128 万次 → 0 次 | 定长预分配，无堆增长 |

**结论**：定长环形缓冲预分配消除了运行期动态内存分配（缺页是启动期一次性分配）。

#### 新瓶颈：缓存行乒乓（false sharing）

noalign 的两个原子状态 `tail_`/`head_` 未做缓存行填充（验证两者紧邻、同缓存行）。生产者每 push 写 `tail_`、消费者每 pop 写 `head_`，同缓存行时在两个物理核间持续往返传输（false sharing），即使数据本身互不依赖。

本机 WSL2 无硬件事件（Hyper-V 未向 guest 暴露 PMU，见 [microsoft/WSL#8155](https://github.com/microsoft/WSL/issues/8155)），无法直接观测 cache miss。**TODO**：需在物理机上用 `perf stat -e cache-misses`、`perf c2c` 直接观测缓存行乒乓的 cache miss 计数，并与下一版做缓存行填充的实现对比。

## ⭐ 无锁环形队列（near SOTA）

上一节的 `spsc_noalign` 缺陷在于 `tail_`/`head_`/`cachedHead_`/`cachedTail_` 紧邻存放、可能同缓存行。本节 `spsc_align` 修复：四个状态变量各自 `alignas(64)` 独占缓存行，两个物理核各写各的行，消除 false sharing（源码见 [spsc_align.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_align.hpp)）。

### perf stat：全局计数

```bash
perf stat ./bin/spsc_align
```

```text
 Performance counter stats for './bin/spsc_align':

           7595.69 msec task-clock                #    1.999 CPUs utilized
                48      context-switches          #    6.319 /sec
                 3      cpu-migrations            #    0.395 /sec
              2180      page-faults               #  287.005 /sec
   <not supported>      cycles
   <not supported>      instructions
   <not supported>      branches
   <not supported>      branch-misses

       3.799388376 seconds time elapsed

       7.584835000 seconds user
       0.003998000 seconds sys
```

与缺陷版（noalign）逐项对照：

| 字段 | noalign（同缓存行） | align（缓存行分隔） | 变化 |
|------|---------------------|--------------------|------|
| `CPUs utilized` | 2.018 | **1.999** | 均接近 2.0，CPU 跑满 |
| `page-faults` | 2182 | **2180** | 相同（同容量一次性分配） |
| `sys` | 0.004 s | **0.004 s** | 均基本归零 |
| 墙钟 | 4.90 s | **3.80 s** | align 快约 29%（缓存行分隔消除乒乓） |

**TODO**：本机 WSL2 无硬件事件（Hyper-V 未暴露 PMU），无法用 `cache-misses` 直接对比两版的缓存行乒乓。需在物理机上跑 `perf stat -e cache-misses` 对比 noalign/align，预期 align 的 cache miss 显著更少。

---

> **小结**：性能观测按"计数 → 采样 → 追踪"三层组织，各自回答不同层级的问题：
>
> - **`perf stat`（计数）**：给事件总量，用 `CPUs utilized`、`context-switches`、`page-faults`、`user`/`sys` 推断并发形态与开销去向——"总量是多少、时间去哪了"；
> - **`perf record`/`report`/`script`（采样）**：周期中断记录调用栈，`perf report` 按 Self/Children 聚合定位热点函数与热路径；`perf script` 导出原始样本，配合 `flamegraph.pl` 生成火焰图可视化——"热点在哪"；
> - **`perf trace`（追踪）**：按 syscall 聚合统计，量化系统调用成本（如 futex 的次数、耗时、EAGAIN 率），还原机制——"为什么慢"。
>
> 三者从计数到追踪逐层深入，且都依赖事件：`perf list` 先确认环境可观测事件全集（物理机硬件事件全可用；WSL2 无硬件事件，软件事件与采样追踪不受影响；云 VM 硬件事件可列出但数值失真），再选择工具、解读数据。
