# 08 · 运行监视（Web MVC）

对齐常见 **C# / .NET Process + GC** 面板：工作集、专用内存、托管堆、回收次数、句柄、CPU、线程、运行时长。HTML 每 **5 秒** 拉一次 `GET /api/gc`。

## 指标对照

| 面板（中文） | HaoLang API | 对标 .NET |
|-------------|-------------|-----------|
| 工作集内存 | `os.Process.workingSetBytes` | `WorkingSet64` |
| 专用内存 | `os.Process.privateBytes` | `PrivateMemorySize64` |
| GC 托管堆 | `gc.GC.liveBytes` | 托管堆大致量级 |
| GC minor ≈ Gen0 | `gc.GC.minorCount` | `CollectionCount(0)` |
| GC major ≈ Gen2 | `gc.GC.majorCount` | `CollectionCount(2)` |
| （无 Gen1） | — | HaoLang 仅 young/old 二分代 |
| 句柄数 | `os.Process.handleCount` | `HandleCount` |
| CPU 使用率 | `os.Process.cpuPercent` | 进程 CPU%（按核归一） |
| 活跃线程数 | `os.Process.threadCount` | `Threads.Count` |
| GC 注册线程 | `gc.GC.registeredThreads` | （STW 参与线程，额外） |
| STW incomplete | `gc.GC.stwIncomplete` | 未齐 park、跳过 sweep（v0.50.4） |
| 运行时长 | `os.Process.uptimeMs` | 进程存活时间 |

栈仍为**保守扫描**，没有逐帧堆栈导出。GC 现状是**协作 STW + pacing**，不是 Go 式并发标记。

## 运行

仓库根：

```powershell
. .\env.ps1
# 若刚改过 runtime：powershell -File script\win\build_runtime.ps1
hao run haolang-example\08-gc-monitor\main.hao
```

打开 <http://127.0.0.1:18090/>

示例用 `serveBossWorkers(4)` + 后台 **4000** 次保活分配作验收：密集分配会触发 STW，但 `/api/gc` 应仍可响应。
**禁止**把预热改回 40 当作「修好了」。
