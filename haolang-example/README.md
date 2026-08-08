# HaoLang 示例库

面向学习者的可运行示例。CI 套件与内部冒烟仍在 `test/`，**不要**把 `test/suite` 当教程。

## 前置

开发树（仓库根）：

```powershell
. .\env.ps1
# 需要时: haobuild；改了 runtime 再 build_runtime.ps1
```

发行包解压后：在**包根**（含 `bin`/`examples`/`stdlib`）执行；路径前缀用 **`examples\`**（不是 `example\`、也不是 `haolang-example\`）。未加 PATH 时用 `.\bin\hao.exe`。
## 目录

| 目录 | 内容 |
|------|------|
| [01-hello](01-hello/) | 最小 Hello |
| [02-features](02-features/) | 语言 / 集合 / 并发单文件 |
| [03-multifile](03-multifile/) | 多文件本地包 `calc` / `shapes` |
| [04-web](04-web/) | MVC：`HttpApp.scan` + 嵌套包 |
| [05-project-local](05-project-local/) | 工程清单 `haoproject.json` + `localReferences`（无 `haopkg.json`） |
| [06-project-haopkg](06-project-haopkg/) | 工程清单 + 仓内 `haopkg.json` + `hao mod tidy` |
| [07-testing](07-testing/) | `hao test` |
| [08-gc-monitor](08-gc-monitor/) | Web MVC：GC/内存监控页（5s 刷新） |

> **`haoproject.json` ≠ `haopkg.json`**：前者是应用工程清单，后者是发布到仓里的包元数据。详见 [`docs/hao命令.md` §4.0](../docs/hao命令.md#40-haoprojectjson-与-haopkgjson-的区别)。

更完整的语法与命令说明见仓库 [`docs/hao语法.md`](../docs/hao语法.md)、[`docs/hao命令.md`](../docs/hao命令.md)（发行包内在 `docs\`）。

## 快速开始

```powershell
hao run haolang-example\01-hello\hello.hao
hao run haolang-example\02-features\basics.hao
hao run haolang-example\03-multifile\main.hao
```
