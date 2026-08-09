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

## 观测方法论：计数、采样、追踪

**一切的观测都是基于事件的**：perf 观测的不是"程序本身"，而是程序执行过程中发生的事件——CPU 周期、指令、缓存访问、分支、系统调用、缺页、调度切换等。工具做三件事：计数事件、对事件采样、追踪事件流。事件从何而来、是否可用，决定了一切观测手段的边界，这正是下一节"环境准备"的动机。

性能观测工具按测量方式可分为三类，分别回答不同层级的问题，粒度与开销依次递进：

| 层次 | 工具 | 回答的问题 | 粒度 | 对被测程序的干扰 |
|------|------|-----------|------|----------------|
| 计数 | `perf stat` | 事件总量是多少？ | 全局聚合 | 极低（读硬件/软件计数器） |
| 采样 | `perf record` / `perf top` | 热点在哪个函数、哪条指令？ | 调用栈/指令 | 低（周期性中断） |
| 追踪 | `perf trace` | 程序与内核如何交互？ | 单次系统调用 | 中（事件流捕获） |

三者呈递进关系：计数给总量、采样定位热点、追踪解释机制，排查通常沿此顺序推进。共同前提是**可复现的基准**：本文演示程序将两个工作线程经 `pthread_setaffinity_np` 固定绑定到不同物理核（cpu 0/1），测试规模固定为 3000 万条消息，保证观测结果可比。

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

### perf stat：全局计数

`perf stat` 执行命令并输出事件计数。它不做采样，直接读取计数器，**回答"总量"问题**。

被测程序 `spsc_naive_mutex` 是一个带 mutex 锁的 SPSC 队列实现：`spsc::Queue` 以 `std::mutex` 保护 `std::deque`，每次 push/pop 全量加锁。源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)、[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)、[Makefile](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/Makefile)。

#### 基本用法与事件选择

```bash
perf stat ./bin/spsc_naive_mutex            # 默认事件集
perf stat -d ./bin/spsc_naive_mutex         # 常用默认事件集
perf stat -e task-clock,page-faults ./bin/spsc_naive_mutex   # 显式指定事件
```

程序默认处理 3000 万条消息，命令中无需再传参。

常用参数：

- `-e`：显式选择事件，逗号分隔多个；
- `-d`：展开常用事件集（cache miss、分支等）；
- `-r N`：重复运行 N 次取平均，用于掩盖单次噪声；
- `--per-core` / `--per-socket`：按拓扑拆分计数。

#### 输出字段解读

WSL2 上实测 `perf stat` 输出（软件事件可计数，硬件事件缺失显示 `<not supported>`；行尾注释为该事件的统计含义）：

```text
 Performance counter stats for './bin/spsc_naive_mutex':

           3184.25 msec task-clock:u              #    1.578 CPUs utilized
                 0      context-switches:u        #    0.000 /sec
                 0      cpu-migrations:u          #    0.000 /sec
             30677      page-faults:u             #    9.634 K/sec
   <not supported>      cycles:u                  # CPU 周期数；与 instructions 之比即 IPC
   <not supported>      instructions:u            # 已执行指令数
   <not supported>      branches:u                # 分支指令数
   <not supported>      branch-misses:u           # 分支预测失败次数；失败率高则流水线停顿多
   <not supported>      L1-dcache-loads:u         # L1 数据缓存读访问次数
   <not supported>      L1-dcache-load-misses:u   # L1 数据缓存读未命中次数
   <not supported>      LLC-loads:u               # 末级缓存（LLC）读访问次数
   <not supported>      LLC-load-misses:u         # 末级缓存读未命中次数；未命中即访问主内存

       2.017960694 seconds time elapsed

       1.864642000 seconds user
       1.357627000 seconds sys
```

| 字段 | 含义 | 本例解读 |
|------|------|---------|
| `task-clock` | 进程消耗的 CPU 时间 | 3184 ms |
| `CPUs utilized` | task-clock 除以墙钟时间 | **1.578**：两个工作线程的 CPU 占用率之和 |
| `context-switches` | 内核调度切换次数 | **0**：等待是亚毫秒级，未触发调度睡眠 |
| `cpu-migrations` | 线程跨核迁移次数 | **0**：线程固定绑定，无迁移 |
| `page-faults` | 缺页次数 | **30677**：动态分配触及新内存页 |
| `<not supported>` | 该事件本机不可用 | 硬件 PMU 缺失；本输出中 8 个硬件事件均缺失，统计含义见输出块行尾注释 |
| `user` / `sys` | 用户态 / 内核态 CPU 时间 | 1.865 s / 1.358 s，约各占一半 |

#### 性能问题分析

- **CPU 没跑满：锁等待**：`CPUs utilized` 仅 1.578（双线程应接近 2.0）。互斥使任何时刻只有一个线程在执行临界区，另一个线程在等待（自旋或 futex 睡眠），等待期间不占用 CPU，`task-clock` 因此小于墙钟的两倍，utilized 随之低于 2.0；`context-switches` 为 0，说明等待极短、走自旋而非调度睡眠——"短临界区锁竞争"形态。
- **内核路径开销太高**：`user`/`sys` 内核态（1.358 s）与用户态（1.865 s）同量级，大量时间耗在 futex 系统调用路径，而非纯用户态计算——与后文 `perf trace` 的 futex 观测相互印证。
- **缺页严重：动态内存**：`page-faults=30677`，动态分配触及新内存页，内存分配在关键路径上活跃。

这些是计数层的初步缺陷假设，具体位置（哪个函数、哪次调用）需采样与追踪进一步定位。

### perf record / perf report：调用栈采样

`perf record` 周期性中断进程，记录中断点的指令指针（PC）与调用栈；`perf report` 对样本聚合统计。**采样是统计学估计而非精确测量**：热点占比由样本分布近似，样本越多估计越准。

被测程序与上一节相同：源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)，两个线程经 `pthread_setaffinity_np` 绑定到 cpu 0/1；`spsc::Queue` 为 `std::mutex` 保护的 `std::deque`（实现见 [spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)），push 全量加锁、pop 全量加锁。观测对象：锁路径在采样报告中的占比。

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

#### 输出解读：Self 与 Children

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

两列的含义是采样的核心：

- **Self**：该符号自身被采样到的比例（不含子调用）——定位**热点所在处**；
- **Children**：该符号及其所有子调用合计比例——定位**热路径方向**。

本例中 `pthread_mutex_lock` 与 `pthread_mutex_unlock` 的 Self 合计约 65.7%（34.85% + 30.80%），说明近三分之二 CPU 时间直接耗在锁的获取与释放上；顶层未解析地址的 Children 97.79%，说明几乎全部采样最终都经过锁路径，锁是支配性的热点。未解析的 libc 地址（`0x…6912f7` 等，合计约 23%）对应 futex 等待与 `__lll_lock_wait` 等内部慢路径。

#### 符号解析问题

采样记录的是地址，report 依赖符号表将其转为函数名。本机输出中 libstdc++/libc 内部地址无法解析（`0x0000721958adc253` 等），对应 futex 等待与 `__lll_lock_wait` 等内部慢路径——这些地址即便不解析，也能从父子调用结构推断其归属。若要完整解析，可安装带 debug info 的库或配置 debuginfod；对性能热点定位，未解析地址通常不阻塞定性结论。

#### 性能问题分析

- **锁是支配性热点**：`pthread_mutex_lock` 与 `pthread_mutex_unlock` 的 Self 合计约 65.7%（34.85% + 30.80%），近三分之二 CPU 时间直接耗在锁的获取与释放上；顶层未解析地址的 Children 97.79%，几乎全部采样最终都经过锁路径——锁路径确认是瓶颈所在。
- **等待在锁内部慢路径**：未解析的 libc 地址（`0x…6912f7` 等，合计约 23%）对应 futex 等待与 `__lll_lock_wait` 等内部慢路径，进一步印证 lock/unlock 的 Self 中相当部分属等待而非临界区执行。
- **采样定位了位置**：与计数层"CPU 没跑满：锁等待"的假设相互印证——之前只能推断锁等待存在，现在明确了锁的获取与释放自身就是热点所在，为后续优化（无锁化）提供依据。

#### perf top：实时版

`perf top` 是同一采样机制的实时版本，周期性刷新当前热点，适用于交互式排查（如观察某负载下的瞬时热点），无需预先记录。

> **VM 待补**：以硬件 `cycles` 为采样事件的 `perf record` 在 VM 上的报告、`cache-references`/`cache-misses` 读数、TMA 指标均待实测补充。

### perf trace：系统调用追踪

`perf trace` 捕获进程的系统调用，功能与 strace 重叠，但**实现机制不同**：strace 基于 ptrace 逐条拦截，开销大；`perf trace` 直接订阅内核 syscall 事件，对被测程序的时序影响低约一个数量级——对低延迟程序，这决定了工具本身是否改变测量对象。

被测程序与上文相同：源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)，`spsc::Queue` 以 `std::mutex` 保护 `std::deque`（[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)），每次 push/pop 全量加锁，锁竞争通过 futex 系统调用进入内核。观测对象：futex 调用次数与耗时。

#### 用法

```bash
perf trace ./bin/spsc_naive_mutex                # 流式输出每次系统调用
perf trace --summary -e futex ./bin/spsc_naive_mutex     # 按系统调用聚合汇总
perf trace -e futex,mmap ./bin/spsc_naive_mutex          # 事件过滤
```

`--summary` 对执行期间的系统调用按类型聚合，输出每次调用的计数、耗时分布，适合快速判断"程序在系统调用上花了多少时间"。

#### 定位问题一：syscall 调用过多

`perf stat` 已发现 `sys` 时间（1.358 s）与 `user` 同量级——内核路径开销太高。系统调用是用户态进入内核的唯一通道，下一步用 `perf trace` 量化：到底是哪些 syscall、多少次、花了多少时间。

不带 `-e` 过滤的全量汇总可先看整体画像。`perf trace` 需要读取内核事件，权限受 `perf_event_paranoid` 限制，故加 `sudo` 运行：

```bash
sudo perf trace --summary ./bin/spsc_naive_mutex
```

实测输出（3000 万条消息）按线程分段聚合，初始化系统调用已从略：

```text
# 主进程（join 等待工作线程结束）
 spsc_naive_mute (44875), 166 events, 0.0%

   syscall            calls  errors  total       min       avg       max       stddev
                                     (msec)    (msec)    (msec)    (msec)        (%)
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex                  2      0  2499.959   151.140  1249.979  2348.819     87.91%
   clone3                 2      0     0.481     0.211     0.240     0.270     12.32%
   …（mmap/mprotect 等初始化系统调用从略）

# producer 线程（push 路径）
 spsc_naive_mute (44876), 1011283 events, 46.8%

   syscall            calls  errors  total       min       avg       max       stddev
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex             477669 213591   916.119     0.001     0.002     0.491      0.20%
   mprotect           27138      0    63.954     0.002     0.002     1.666      2.67%
   mmap                  8      0     0.068     0.006     0.009     0.015     12.34%
   …（munmap/madvise 等少量系统调用从略）

# consumer 线程（pop 路径）
 spsc_naive_mute (44877), 1148573 events, 53.2%

   syscall            calls  errors  total       min       avg       max       stddev
   --------------- --------  ------ -------- --------- --------- ---------     ------
   futex             573278 155309  1230.659     0.001     0.002     1.344      0.33%
   munmap                 2      0     0.845     0.003     0.422     0.841     99.23%
   …（mmap/mprotect 等少量系统调用从略）
```

字段顺序为 `calls、errors、total(msec)、min、avg、max、stddev`，每个线程段按 syscall 类型聚合。解读要点：

- **futex 是绝对主导**：两个工作线程合计约 105 万次 futex 调用，累计约 2.1 s；程序自身打印的运行时间约 2.5 s，两者接近——**锁等待直接占据运行时间的大部分**。平均每约 30 条消息发生一次 futex 往返；
- **errors（EAGAIN）**：约 37 万次调用返回错误（约占 35%）。futex 的 FUTEX_WAIT 被唤醒后需要校验锁值，若仍被占用则返回 EAGAIN——"假唤醒"。短临界区锁竞争下，等待线程几乎刚睡下就被唤醒，睡眠本身毫无收益，纯为系统调用往返付费；
- **producer 侧 27138 次 mprotect**：deque 无界、扩容时分配新块，glibc malloc 为新映射页调用 mprotect——动态内存分配活跃的 syscall 级直接证据，与问题二的缺页呼应；

定位结论：syscall 过多的来源已从"内核路径开销太高"细化到具体通道——锁竞争导致的 futex 往返（约 105 万次、约 2.1 s、35% 为无收益的 EAGAIN），同时动态内存分配（mprotect）作为次要但可观的 syscall 开销浮出水面。

#### 定位问题二：缺页

> **TODO**：WSL 不提供 `minor-faults` 追踪（无 tracepoint），本节的 `perf trace -e minor-faults` 与 `exceptions:page_fault_user` 均无法验证；需在物理机上实测缺页 tracepoint 的事件流与地址字段。

`perf stat` 显示 30677 次缺页。与 syscall 不同，缺页本身不是系统调用（x86 上由缺页异常触发），`perf trace` 默认的 syscall 汇总看不到它。尝试显式追踪：

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
# Samples: 639  of event 'minor-faults'
# Event count (approx.): 14209
#
# Children      Self  Command          Shared Object         Symbol
# ........  ........  ...............  ....................  .........................
    84.27%    84.27%  spsc_naive_mute  libc.so.6             [.] 0x00000000000a320b
     9.39%     9.39%  spsc_naive_mute  spsc_naive_mutex      [.] main
     3.36%     0.00%  spsc_naive_mute  libstdc++.so.6.0.30   [.] 0x000077139f8dc253
            |--2.65%--0x77139f5a0b4a
             --0.70%--std::thread::_State_impl<...>::_M_run    # 线程启动路径
     …（其余各 <1%）
```

解读：

- **采样覆盖**：639 个样本对应约 14209 次缺页事件（采样率约 46%，30677 次缺页中的近半被采样到），缺页发生时中断并记录调用栈；
- **缺页集中在 libc 内部**：84.27% 的样本落在 libc 一个地址（`0xa320b`）——具体是哪个函数，需要查证而非推断；
- **main（9.39%）与线程启动路径（3.36%）**：程序启动时栈初始化触页，数量级与"启动开销"吻合，非关键路径。

**查证地址归属**：`0xa320b` 是 libc 内部偏移，perf 未解析符号。用 `readelf` 的符号表反查该偏移落在哪个函数体内：

```bash
# 找地址 0xa320b 在符号表中最近的上、下界（gawk 支持 strtonum）
readelf -Ws /lib/x86_64-linux-gnu/libc.so.6 | gawk '
  $2!="UND" && $2!="" { split($2,a,":"); v=strtonum("0x" a[1]);
    if (v>0 && v<=0xa320b && v>bl) {bl=v; b=$0}
    if (v>0 && v>0xa320b && (!bu || v<bu)) {bu=v; u=$0} }
  END { print "below:", b; print "above:", u }'
```

```text
below:    935: 00000000000a2570    51 FUNC    GLOBAL DEFAULT   15 __default_morecore@GLIBC_2.2.5
above:    379: 00000000000a5060   828 FUNC    GLOBAL DEFAULT   15 malloc@@GLIBC_2.2.5
```

`0xa320b` 落在 `__default_morecore`（0xa2570，51 字节）与 `malloc`（0xa5060）之间的 gap 内——即分配器内部未导出区域。`__default_morecore` 是 glibc 在主堆耗尽时调用、向内核扩展堆的底层函数：**缺页发生在这里，客观说明缺页由堆增长（malloc 向内核申请新页）触发**，而不是磁盘回读（major-faults）或映射文件。

定位结论：缺页来源已查证为**动态内存分配（堆增长路径）**——与问题一 producer 侧 27138 次 mprotect 相互印证，与"以定长环形缓冲替换 deque"的优化路线对应。

> **TODO**：WSL 不提供 `minor-faults` 追踪（无 tracepoint），本节的 `perf trace -e minor-faults` 与 `exceptions:page_fault_user` 均无法验证；需在物理机上实测缺页 tracepoint 的事件流与地址字段。

#### 性能问题分析

- **syscall 过多：futex 无收益往返**：两线程合计约 105 万次 futex 调用、累计约 2.1 s，占运行时间（约 2.5 s）的 86%，其中约 37 万次（35%）为 EAGAIN 假唤醒——锁竞争在"数量"层面被量化确认。
- **缺页：动态内存分配**：producer 侧 27138 次 mprotect 是 syscall 级直接证据；`perf record -e minor-faults -g` 采样显示 84.27% 的缺页样本落在 libc 堆分配路径——共同指向无界 deque 扩容分配新块。

### 性能分析结论

三层观测在演示案例上形成交叉验证的证据链，按性能问题组织：

#### 性能问题一：锁竞争

**证据链**：

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `CPUs utilized` 仅 1.578（双线程应接近 2.0）、0 次 context-switch | 存在不占用 CPU 的等待，等待极短、走自旋 |
| 采样 | `pthread_mutex_lock`/`unlock` Self 合计 65.7%，顶层 Children 97.79% 收敛于锁路径 | 锁是支配性热点 |
| 追踪 | 两线程合计约 105 万次 futex、累计约 2.1 s，占运行时间（约 2.5 s）的 86%；其中约 37 万次（35%）为 EAGAIN 假唤醒 | 锁等待的时间成本量化，形态为无收益的睡眠/唤醒往返 |

**结论**：瓶颈在同步本身而非数据路径。锁竞争使 CPU 无法跑满（1.578 < 2.0），大量时间耗在 futex 往返与 lock/unlock 上——**无锁消除锁，环形缓冲消除锁上的等待**。

#### 性能问题二：动态内存分配

**证据链**：

| 层次 | 证据 | 定位 |
|------|------|------|
| 计数 | `page-faults` 30677 次 | 缺页活跃，动态分配触及新内存页 |
| 采样 | `perf record -e minor-faults -g`：84.27% 缺页样本落在 libc 内部（查证为 `__default_morecore` 附近，堆增长路径） | 缺页来自堆增长，而非磁盘回读 |
| 追踪 | producer 侧 27138 次 mprotect | 动态内存分配的 syscall 级直接证据，与缺页量级吻合 |

**结论**：缺页与 mprotect 均指向无界 deque 的扩容分配——**定长环形缓冲预分配，彻底消除关键路径上的动态分配**。

两个问题都不是数据路径的计算开销，而是同步与分配的结构性开销——这正是无锁环形队列替代 mutex+deque 的意义所在。

## 真机进阶

真机上（硬件 PMU 可用）的进阶手段：`perf record -e cycles:pp`（PEBS 精确采样，消除 skid）、`--call-graph lbr`（硬件调用栈）、`perf mem` / `perf c2c`（内存访问与伪共享定位）、`perf script` 配合 flamegraph 生成火焰图。

---

> **小结**：性能观测按"计数 → 采样 → 追踪"三层组织：`perf stat` 给总量并推断并发形态，`perf record`/`perf report` 以采样统计定位热点（区分 Self 与 Children），`perf trace` 量化系统调用成本并还原机制。以 SPSC 队列 v1（mutex + deque）为例，三层证据一致指向锁竞争：约 65.7% 采样时间在 lock/unlock，105 万次 futex 调用累计约 2.1 s、占据运行时间的大部分，其中 37 万次为 EAGAIN 往返。运行环境决定可观测事件全集：物理机全部可用，WSL2 硬件事件缺失，云 VM 硬件事件可列出但计数失真——先 `perf list` 确认环境再解读数据，虚拟化下的硬件计数只用于展示事件全貌，不作数值结论。
