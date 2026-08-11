# 08 · 运行监视（Web MVC）

对齐常见 **C# / .NET Process + GC** 面板：工作集、专用内存、托管堆、回收次数、句柄、CPU、线程、运行时长。HTML 每 **5 秒** 拉一次 `GET /api/gc`。

## 指标对照

| 面板（中文） | HaoLang API | 对标 .NET |
|-------------|-------------|-----------|
| 工作集内存 | `os.Process.workingSetBytes` | `WorkingSet64` |
| 专用内存 | `os.Process.privateBytes` | `PrivateMemorySize64` |
| GC 托管堆（堆压） | `gc.GC.heapBytes` | 当前堆用户区合计（看堆压用这个） |
| GC 上次 major 存活 | `gc.GC.liveBytes` | 成功 major 后才更新（可能为 0） |
| GC minor ≈ Gen0 | `gc.GC.minorCount` | `CollectionCount(0)` |
| GC major ≈ Gen2 | `gc.GC.majorCount` | `CollectionCount(2)` |
| （无 Gen1） | — | HaoLang 仅 young/old 二分代 |
| 句柄数 | `os.Process.handleCount` | `HandleCount` |
| CPU 使用率 | `os.Process.cpuPercent` | 进程 CPU%（按核归一） |
| 活跃线程数 | `os.Process.threadCount` | `Threads.Count` |
| GC 注册线程 | `gc.GC.registeredThreads` | （STW 参与线程，额外） |
| STW incomplete | `gc.GC.stwIncomplete` + Root/Term 分相 | 软 STW 未齐；与 markAbort 同步涨且 abortRoot≈总量 ⇒ 根握手税（工程债） |
| mark abort | `markAbortCycles` + Root/Term/ParkWd | abort MARK 分相（v0.55.52）；parkWd 涨看 watchdog |
| last STW snap | `lastStwMissing`/`lastStwOsBlockMissing` | 末次未齐缺几人、其中 os_block 数（定位用） |
| miss park tid/龄 | `lastMissTid0`/`lastMissMaxAgeMs` | 末次缺 park 线程 tid + 距上次 SP 龄（v0.55.53；非 PC→源码） |
| finalizer diag | `lastFinalizerDiag`/`finalizerSkipAbort`/`finalizerLiveAtSweep` | runs=0 时区分「仍活」vs「abort 跳过」（v0.55.53） |
| park watchdog | `parkWatchdogTrips`（stats） | park 超时强行放行次数（v0.55.11；正常应为 0） |
| 突然全 pending | 进程不退、`parkWd=0` | 查是否为旧 binary；须 **v0.55.12** term-drain 放锁 |
| 运行时长 | `os.Process.uptimeMs` | 进程存活时间 |

GC（v0.55+）：Hao 帧走 **shadow 精确根**；park 扫 GPR + 有界 C 叶（v0.55.18）；minor 扫栈上 old 的 young 子；晋升挂 remset。协作软 STW + **并发 mark**。`/api/*` 默认短连接。详文 [`docs/IR与GC契约.md`](../../docs/IR与GC契约.md)。

**压测注意**：进程仍在、内存很小、HTTP 一直不返回 → 优先查分相 `markAbortRoot`/`Term`/`ParkWd` 与 `lastStw*`（GC/STW 楔死），不是 OOM。**两格同步涨且仍在 collect ≠ 必修 bug**（根未齐 abort 是故意的）。须用**本机新编** `hao build` + 新 `libhaort.a`，勿用旧 `08-gc-monitor.exe`。

## 运行

仓库根：

```powershell
. .\env.ps1
# 若刚改过 runtime：powershell -File script\win\build_runtime.ps1
hao run haolang-example\08-gc-monitor\main.hao
```

打开 <http://127.0.0.1:18090/>

示例用 `serveBossWorkers(4)` + 后台 **4000** 次保活分配作验收：密集分配会触发 STW，但 `/api/gc` 应仍可响应。STW 压力高时预热自动拉长休眠（v0.55.11）。
**禁止**把预热改回 40 当作「修好了」。
