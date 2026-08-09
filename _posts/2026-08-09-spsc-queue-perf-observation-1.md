---
title: "性能观测工具使用：以SPSC优化为例"
date: 2026-08-09 14:00:00 +0800
model: DeepSeek-V4-Flash
tool: Claude Code CLI
---

性能优化的前提是准确的观测。本文以 Linux 官方性能剖析工具 perf 为主线，介绍性能观测的三层方法论——**全局计数、调用栈采样、系统调用追踪**——并给出每层工具的用法、输出解读与适用场景。本文以 SPSC 的优化为例演示性能观测工具的使用方法，全部命令与输出（包括失真数据）均为真实运行结果。

- 目录
{:toc}

> **环境提示**：本文在 WSL2 与 Linux VM 上运行。虚拟化环境下部分数值会失真，失真数据均在文中标注。

## 观测方法论：计数、采样、追踪

性能观测工具按测量方式可分为三类，分别回答不同层级的问题，粒度与开销依次递进：

| 层次 | 工具 | 回答的问题 | 粒度 | 对被测程序的干扰 |
|------|------|-----------|------|----------------|
| 计数 | `perf stat` | 事件总量是多少？ | 全局聚合 | 极低（读硬件/软件计数器） |
| 采样 | `perf record` / `perf top` | 热点在哪个函数、哪条指令？ | 调用栈/指令 | 低（周期性中断） |
| 追踪 | `perf trace` | 程序与内核如何交互？ | 单次系统调用 | 中（事件流捕获） |

三者呈递进关系：计数给总量、采样定位热点、追踪解释机制，排查通常沿此顺序推进。共同前提是**可复现的基准**：本文演示程序将两个工作线程经 `pthread_setaffinity_np` 固定绑定到不同物理核（cpu 0/1），测试规模固定为 3000 万条消息，保证观测结果可比。演示程序源码见 [main.cpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/main.cpp)、[spsc_naive_mutex.hpp](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/include/spsc_naive_mutex.hpp)、[Makefile](https://github.com/4thcalabash/qd-knowledge/blob/master/_demo/spsc_perf_test/Makefile)。

## 环境准备：事件可用性检查

`perf list` 列出本机全部可用事件，事件分两类：

- **硬件事件**（`cycles`、`instructions`、`cache-misses` 等）：来自 CPU 的 PMU 计数器，需硬件支持；
- **软件事件**（`cpu-clock`、`page-faults`、`context-switches` 等）：由内核计数，任何环境可用。

硬件事件是否可用、可用到哪一层，取决于运行环境。本次在两种环境实测，恰好呈现"缺失"与"存在但失真"两档：

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

两档对比：

| 环境 | 硬件事件 | 说明 |
|------|---------|------|
| WSL2 | 完全缺失 | 硬件级观测（cache miss 率、PEBS、LBR、perf c2c）不可用 |
| 云 Linux VM | 核心事件齐全，cache 事件部分缺失 | 缺失 LLC（末级缓存）与 L1-dcache-stores——多数虚拟化环境不向 guest 暴露 LLC 计数 |
| 物理机 | 全部可用且数值可信 | 理想参照，本文未实测 |

注意：**事件存在 ≠ 数值可信**。VM 上事件能列出、能计数，但读数失真（见 perf stat 一节）。排查前先 `perf list` 确认可用事件，同时警惕虚拟化环境下把坏读数当作性能结论。

## perf stat：全局计数

`perf stat` 执行命令并输出事件计数。它不做采样，直接读取计数器，**回答"总量"问题**。

### 基本用法与事件选择

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

### 输出字段解读

```text
 Performance counter stats for './bin/spsc_naive_mutex':

           3184.25 msec task-clock:u              #    1.578 CPUs utilized
                 0      context-switches:u        #    0.000 /sec
                 0      cpu-migrations:u          #    0.000 /sec
             30677      page-faults:u             #    9.634 K/sec
   <not supported>      cycles:u
   <not supported>      instructions:u
   <not supported>      branches:u
   <not supported>      branch-misses:u
   <not supported>      L1-dcache-loads:u
   <not supported>      L1-dcache-load-misses:u
   <not supported>      LLC-loads:u
   <not supported>      LLC-load-misses:u

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
| `<not supported>` | 该事件本机不可用 | 硬件 PMU 缺失 |
| `user` / `sys` | 用户态 / 内核态 CPU 时间 | 1.865 s / 1.358 s，约各占一半 |

`CPUs utilized` 与 `context-switches` 两个字段组合即可推断并发形态：双线程程序该值应接近 2.0，实测仅 1.578，说明线程存在大量不占用 CPU 的等待；同时 0 次 context-switch 说明等待极短、走的是自旋而非调度睡眠。这已指向"短临界区锁竞争"的形态。

`user`/`sys` 时间值得注意：内核态时间（1.358 s）与用户态时间（1.865 s）同量级，说明大量时间耗在内核路径（futex 系统调用），而非纯用户态计算——与后文 `perf trace` 的 futex 观测相互印证。

### 硬件事件计数：事件在，数值失真

同一程序在云 VM 上运行，请求计数硬件事件（cycles、instructions、branches、branch-misses、stalled-cycles-frontend/backend 等）。以下输出中标注 **失真** 的为虚拟化下不可信的计数，仅展示事件输出形态：

```text
 Performance counter stats for './bin/spsc_naive_mutex':

         10,453.70 msec task-clock                #    1.878 CPUs utilized
             9,707      context-switches          #    0.929 K/sec
                 2      cpu-migrations            #    0.000 K/sec
            13,837      page-faults               #    0.001 M/sec
755,777,057,308,007,040      cycles                    # 72297590.490 GHz    失真   (66.83%)
750,726,875,787,298,432      stalled-cycles-frontend   #   99.33% frontend cycles idle    失真   (67.28%)
816,092,403,685,446,144      stalled-cycles-backend    #  107.98% backend cycles idle    失真   (67.39%)
814,215,035,424,200,960      instructions              #    1.08  insn per cycle    失真
                                                  #    1.00  stalled cycles per insn  (66.58%)
809,042,397,513,195,008      branches                  # 77392949916.222 M/sec    失真   (65.77%)
816,518,889,222,270,208      branch-misses             #  100.92% of all branches    失真   (66.14%)

       5.566710730 seconds time elapsed

       1.007485000 seconds user
       5.223233000 seconds sys
```

三处"物理不可能"信号（行尾 `(66.83%)` 是计数器复用比例，见下）：

- **比例超过 100%**：`branch-misses 100.92%`、`stalled-cycles-backend 107.98%`——任何真实硬件都不会产生超过 100% 的比例；
- **频率离谱**：cycles 折算 72297590 GHz（约 7 万 GHz），远超任何 CPU 主频；
- **各计数器同量级**：cycles / instructions / branches / stalled-* 全部落在 7.5e17 ~ 8.2e17——真实情况下这几者必然相差数量级。

对照软件事件：task-clock（10.4 s）、context-switches（9707）、page-faults（13837）均与 WSL2 同形态、数值合理——**失真只发生在硬件 PMU 路径**，内核计数的软件事件不受影响。这是虚拟化环境下硬件计数的完整签名：**事件可列出、输出格式正确、数值不可信**。读到此类输出应首先怀疑计数器本身，而不是程序。

环境差异本身也是观测结论的一部分：同程序在 WSL2 上墙钟约 2.0 s、VM 上约 5.6 s；VM 上 `sys`（5.22 s）远高于 `user`（1.01 s），futex 往返在内核路径的成本被放大；context-switches 在 WSL2 为 0，VM 上为 9707——云 VM 与宿主共享物理核，guest 线程易被周期性抢占。虚拟化环境的时间绝对数值不具备跨环境可比性。

行尾复用比例补充：请求的事件多于可用硬件计数器时，perf 分时复用计数器、按实际计数时长折算（multiplexing），`(66.83%)` 即占用计数器的时长比例，比例越低折算误差越大。VM 上可用硬件计数器少于事件数，复用必然发生，是读数失真的又一叠加因素。

### 局限

计数只给总量，不回答位置问题——`page-faults=30677` 无法告知缺页发生在哪次调用；`task-clock` 无法告知时间花在哪个函数。定位需要下一层：采样。

## perf record / perf report：调用栈采样

`perf record` 周期性中断进程，记录中断点的指令指针（PC）与调用栈；`perf report` 对样本聚合统计。**采样是统计学估计而非精确测量**：热点占比由样本分布近似，样本越多估计越准。

### 采样原理要点

- 采样事件默认 `cycles`（硬件），也可指定任意事件；事件不可用时自动回退到软件事件 `cpu-clock`；
- 每次中断记录 PC，通常同时记录调用栈；样本数取决于运行时间与采样频率；
- **skid（稀释）效应**：从事件触发到中断处理完成有延迟，记录的 PC 可能已越过触发事件的指令若干条，热点可能偏移数条指令；`-e cycles:pp`（PEBS，硬件精确采样）可消除，但依赖硬件支持。

### 常用参数

```bash
perf record -g ./bin/spsc_naive_mutex          # 记录调用栈（默认 -g 走帧指针）
perf record --call-graph fp ./bin/spsc_naive_mutex     # 帧指针回溯：要求编译带 -fno-omit-frame-pointer
perf record --call-graph dwarf ./bin/spsc_naive_mutex  # DWARF 回溯：支持优化代码，开销大
perf record --call-graph lbr ./bin/spsc_naive_mutex    # 硬件 LBR：开销极低、无 skid，栈深受限
perf record -a                                  # 全系统采样
```

调用栈获取方式的选择与编译方式绑定：帧指针方式要求二进制保留帧指针（`-fno-omit-frame-pointer`，`-O2` 默认省略）；DWARF 方式不要求但开销大一个数量级。低延迟环境惯用帧指针方式，代价极小。

### 输出解读：Self 与 Children

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

### 符号解析问题

采样记录的是地址，report 依赖符号表将其转为函数名。本机输出中 libstdc++/libc 内部地址无法解析（`0x0000721958adc253` 等），对应 futex 等待与 `__lll_lock_wait` 等内部慢路径——这些地址即便不解析，也能从父子调用结构推断其归属。若要完整解析，可安装带 debug info 的库或配置 debuginfod；对性能热点定位，未解析地址通常不阻塞定性结论。

### perf top：实时版

`perf top` 是同一采样机制的实时版本，周期性刷新当前热点，适用于交互式排查（如观察某负载下的瞬时热点），无需预先记录。

> **VM 待补**：以硬件 `cycles` 为采样事件的 `perf record` 在 VM 上的报告待实测补充——硬件计数器失真时，采样地址是否可信（取决于中断是否真实触发、样本是否真实产生）值得单独验证。VM 上 `cache-references`/`cache-misses` 读数、TMA 指标同样待补。

## perf trace：系统调用追踪

`perf trace` 捕获进程的系统调用，功能与 strace 重叠，但**实现机制不同**：strace 基于 ptrace 逐条拦截，开销大；`perf trace` 直接订阅内核 syscall 事件，对被测程序的时序影响低约一个数量级——对低延迟程序，这决定了工具本身是否改变测量对象。

### 用法

```bash
perf trace ./bin/spsc_naive_mutex                # 流式输出每次系统调用
perf trace --summary -e futex ./bin/spsc_naive_mutex     # 按系统调用聚合汇总
perf trace -e futex,mmap ./bin/spsc_naive_mutex          # 事件过滤
```

`--summary` 对执行期间的系统调用按类型聚合，输出每次调用的计数、耗时分布，适合快速判断"程序在系统调用上花了多少时间"。

### 输出解读：以 futex 为例

锁竞争的底层通道是 futex 系统调用。`perf trace --summary -e futex ./bin/spsc_naive_mutex` 实测输出（3000 万条消息）：

```text
 spsc_naive_mute (producer)  878215 events
   futex          438347 197472   681.029   0.001   0.002    0.327    # 加锁被占时的阻塞等待

 spsc_naive_mute (consumer) 1084498 events
   futex          541421 164789   827.190   0.001   0.002    0.914    # 加锁等待 + 空队列等待
```

字段顺序为 `calls、errors、total(msec)、min、avg、max`。解读要点：

- **calls**：两个工作线程合计约 98 万次 futex 调用，累计约 1.5 s；程序自身打印的运行时间约 2.7 s，两者同量级——**锁等待直接占据运行时间的一半以上**。平均每约 30 条消息发生一次 futex 往返；
- **errors（EAGAIN）**：约 36 万次调用返回错误（约占 37%）。futex 的 FUTEX_WAIT 被唤醒后需要校验锁值，若仍被占用则返回 EAGAIN——"假唤醒"。短临界区锁竞争下，等待线程几乎刚睡下就被唤醒，睡眠本身毫无收益，纯为系统调用往返付费；
- 结合 `perf stat` 的 0 次 context-switch：等待是亚毫秒级的自旋与 FUTEX_WAIT 往返混合，从未进入调度睡眠。

## 从观测数据到优化决策

三层观测在演示案例上形成交叉验证的证据链：

| 观测 | 证据 | 结论 |
|------|------|------|
| `perf stat` | CPUs utilized 1.578、0 次 context-switch、30677 次缺页、sys 时间与 user 同量级 | 存在非 CPU 等待；等待极短；动态分配活跃；大量时间在内核态 |
| `perf record` | lock/unlock Self 合计 65.7%，Children 收敛于锁路径 | 锁是支配性热点 |
| `perf trace` | 98 万次 futex、累计约 1.5 s、36 万次 EAGAIN | 锁等待的时间成本量化，形态为无收益的睡眠/唤醒往返 |

结论：**瓶颈在同步本身而非数据路径**。据此制定优化路线：先以定长环形缓冲替换 deque 隔离"分配"变量，再以无锁队列消除锁竞争，最后以批量传输降低原子操作次数——每一步重跑同一观测流程对比。

### 观测的常见陷阱

- **样本量不足**：采样是统计估计，运行时间过短（如低于百毫秒）样本过少，报告失真；保证足够的运行时长或消息量。
- **工具自身的干扰**：追踪类工具（strace 尤甚）会改变被测程序的时序，观察结果不能直接等同于无工具状态；计数与采样干扰最小，优先使用。
- **事件可用性未确认**：硬件事件缺失时命令静默回退或报 `<not supported>`，先 `perf list` 确认，避免误读。
- **虚拟化计数失真**：事件存在 ≠ 数值可信（见"硬件事件计数"一节）。识别信号：比例超 100%、频率超物理可能、各计数器同量级。此时硬件计数只用于展示可观测事件，不能作性能结论；软件事件与调用栈采样通常仍可用。

## 工具选型速查

| 要回答的问题 | 工具 | 命令示例 |
|-------------|------|---------|
| 事件总量（时间、缺页、切换） | `perf stat` | `perf stat -d ./app` |
| 热点函数 / 调用栈 | `perf record` + `perf report` | `perf record -g ./app` |
| 实时热点 | `perf top` | `perf top -p <pid>` |
| 系统调用行为与耗时 | `perf trace` | `perf trace --summary -e futex ./app` |
| 可用事件清单 | `perf list` | `perf list` |

上表在物理机上全部可用；WSL2 缺全部硬件事件（采样自动回退软件事件）；云 VM 事件可列出但硬件计数失真。环境差异先行确认（`perf list`），再选择工具与解读读数。

真机上（硬件 PMU 可用）的进阶手段：`perf record -e cycles:pp`（PEBS 精确采样，消除 skid）、`--call-graph lbr`（硬件调用栈）、`perf mem` / `perf c2c`（内存访问与伪共享定位）、`perf script` 配合 flamegraph 生成火焰图。

---

> **小结**：性能观测按"计数 → 采样 → 追踪"三层组织：`perf stat` 给总量并推断并发形态，`perf record`/`perf report` 以采样统计定位热点（区分 Self 与 Children），`perf trace` 量化系统调用成本并还原机制。以 SPSC 队列 v1（mutex + deque）为例，三层证据一致指向锁竞争：约 65.7% 采样时间在 lock/unlock，98 万次 futex 调用累计约 1.5 s、占据运行时间一半以上，其中 36 万次为 EAGAIN 往返。运行环境决定可观测事件全集：物理机全部可用，WSL2 硬件事件缺失，云 VM 事件存在但数值失真——先 `perf list` 确认环境再解读数据，虚拟化下的硬件计数只用于展示事件全貌，不作数值结论。
