# HaoLang

一门基于 ANTLR4 + LLVM 从零构建的现代静态类型编译型语言。

- 源码后缀：`.hao`
- 编译器命令：`hao`（对标 `go` 命令行）
- 目标：原生机器码、静态链接、单文件绿色分发、可自举
- 当前版本：**v0.55.44**（定位发现十期：下标 dbg.value；GPR VERIFY；recycle TRACE；方法 try 根冒烟）
- 测试基线：`test/suite` **1011** 行 stdout + 退出码 0（`test.sh` 含 loc/spill 冒烟）；反向 `script/win/negcheck.ps1` 28/28；`hao version` = 0.55.44
- **包仓**：`HAO_REGISTRY`=源，`HAO_REPO`=本地仓（默认 `~/.hao/repo`）；测试规范：私服 HTTP + `HAO_REPO=repo/LocalRepo`——见 [`docs/hao命令.md`](docs/hao命令.md) §4
- **下一批（默认）**：扩 IROps（cast/gep/select）清 Expr 残留——见 [`记忆文档.md`](记忆文档.md) 第 10 章

---

## 文档怎么用

| 文档 | 给谁 | 写什么 |
|------|------|--------|
| [`README.md`](README.md)（本文件） | 用户 | 介绍、快速开始、命令表、限制摘要、构建入口（详文链 docs） |
| [`docs/hao命令.md`](docs/hao命令.md) | 用户 | **`hao` 命令详解**：build/run/mod/test/fmt…；**包管理**参数/环境变量/生效方式；`hao test` 对标 `go test` |
| [`docs/hao语法.md`](docs/hao语法.md) | 用户 | **语法与语义说明**（原理、限制）；对标 Go / Java / C# |
| [`记忆文档.md`](记忆文档.md) | 接手 AI | **工作规则**、设计决策摘要、代码地图、下一批 |
| [`docs/IR与GC契约.md`](docs/IR与GC契约.md) | 改 IRGen / GC | **埋点、流程、规则、指标**（GC 权威详文） |
| [`docs/项目时间线/`](docs/项目时间线/索引.md) | 查历史 | 各版本做了什么（按十位合订，如 `0.20.*`～`0.29.*`） |
| [`docs/坑债.md`](docs/坑债.md) | 排障 / 清债 | 踩坑表 + 将就债 |
| [`docs/文档治理.md`](docs/文档治理.md) | 写文档时 | 职责边界、批末写哪里、禁止事项 |

**贡献者硬性约定**（全文见记忆文档第 1 章）：全程中文；测试绿后立刻按文档治理更新；改源码先 `.bak`；不做将就。

---

## 一、这是什么

用 C++17 写成的编译型语言，对标 Go / Kotlin / Java 的语法与能力。核心特点：

- **原生 & 零依赖**：编译为原生机器码，静态链接，单文件分发，无运行时依赖。
- **现代语法**：`val/var`、类型推断、空安全 `T?`、`when`、模板字符串、Lambda/闭包、泛型（单态化零开销）。
- **自带 GC**：见抬头与 [`docs/IR与GC契约.md`](docs/IR与GC契约.md)。
- **自带系统库**：网络 `net`（含 Http/MVC + **`scan`/`scanWhere`/`registerNewArgs`**）、文件/路径 `os`、并发 `sync`、集合 `collections`（含 LinkedList/TreeMap/ConcurrentHashMap 等）、**`gc`**（手动回收与统计）、异常 `exception`、输出 `fmt`、基础包装 `lang`、`regex`/`json`、反射 **`findTypes*`/`newInstanceArgs`** 等。
- **可交叉编译**：Windows 上直接产出 Linux（musl 静态链接）可执行文件。
- **对接 C**：extern C 声明 + 链接外部库/系统库 + 直接写 C 源码。

开发管线：`ANTLR4 词法/语法 → 自研 C++ IRGen 生成 LLVM IR → clang + lld 链接 → 原生可执行文件`。

---

## 二、当前进度

| Stage | 内容 | 状态 |
|-------|------|------|
| 0-3 | 目录结构 / 工具链 / 构建系统 / 语法 | ✅ 完成 |
| 4 | 语法定义与解析 | ✅ 完成 |
| 5 | 语义分析（类型/符号/继承/可见性） | ✅ 完成 |
| 6 | LLVM IR 生成（变量/控制流/类/异常/GC/闭包/泛型） | ✅ 完成 |
| 7 | `hao` 工具链子命令 | ✅ build/run/emit/parse/tokens/mod/**test（TestXxx）/fmt**/clean/env/version |
| 8 | 标准库（collections/…/lang/**Bit**/os/**Path**/math/**gc**…） | ✅ 完成 |
| 9 | 交叉编译（用户程序） | ✅ linux-amd64 |
| 10 | 自举 | ⬜ 待开始 |

**已完成的里程碑**（详见 [`docs/项目时间线/`](docs/项目时间线/索引.md)）：
… → **v0.48.0** `HAO_REPO` → **v0.49.0** Web 补全（当时基线 **972**，历史；当前验收见抬头 **1011**）。

---

## 三、快速开始

**前置**：Windows x64。编编译器需 **MSVC STL**（clang 自动探测本机 VS，**不是** LLVM 自带，**不需要** MinGW）。`setup_env` 会检测，没有则下载安装 VS Build Tools（大包、需管理员）。

```powershell
# 首次（或新机器）：scoop + MSVC + llvm / win CRT / linux sysroot
# GitHub：先直连，再 https://ghfast.top 镜像，仍不行才用 HTTP 代理（proxy.local.ps1）
powershell -ExecutionPolicy Bypass -File script\setup_env.ps1
# 仅补 MSVC：powershell -File script\win\fetch_msvc.ps1

. .\env.ps1          # 加载环境（注意前面的点和空格）
haoenv               # 环境自检
haobuild             # 构建编译器
powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1

# 运行示例（教学示例库；CI 套件仍在 test/）
.\output\hao.exe run haolang-example\01-hello\hello.hao
.\output\hao.exe run haolang-example\02-features\basics.hao
```

`haobuild` 等价于（路径相对仓库根）：
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="$PWD/lib/llvm/bin/clang++.exe" `
  -DCMAKE_RC_COMPILER="$PWD/lib/llvm/bin/llvm-rc.exe"
cmake --build build --parallel 8
```

**已可用命令**（详解见 [`docs/hao命令.md`](docs/hao命令.md)）：
```
hao run    <file或目录...>                编译并运行
hao build  <file或目录...>                编译为可执行文件
hao build  <dir> --target linux-amd64     交叉编译到 Linux
hao emit   <file或目录...>                生成 LLVM IR (.ll)
hao parse  <file.hao>                     打印语法树
hao tokens <file.hao>                     打印词法记号流
hao mod init [模块路径]                   创建 haoproject.json
hao mod tidy [项目目录]                   解析 dependencies → 全局缓存 + lock
hao test [路径...] [-v] [-run regexp]     跑 *_test.hao 中的 TestXxx（对标 go test）
hao fmt [-w|--check] <路径...>            空白+4空格缩进（不断行 pretty-print）
hao clean                                 清理构建产物
hao env / hao version                     环境与版本信息
```

- `hao run/build` 接受**文件或目录**：传文件只编译列出的文件（`go run a.go b.go` 语义）；传目录编译该目录全部 `.hao`（**不含** `*_test.hao`）。
- `hao test`：发现 `func TestXxx(t: testing.T)`，合成入口运行；**不跑业务 main**。详见 [命令文档 §5](docs/hao命令.md#5-hao-test对标-go-test--v042) 与记忆文档 **5.16**。
- `hao fmt`：去行尾空白、统一 LF、末换行、按 `{}` 深度 4 空格缩进；目录递归；`-w`/`--check`；不断行重排。
- 入口旁若有 **`haoproject.json`**（工程清单）：`localReferences` + `dependencies`；仓内发布包元数据是 **`haopkg.json`**（二者不同，见 [命令文档 §4.0](docs/hao命令.md#40-haoprojectjson-与-haopkgjson-的区别)）。设计权威：记忆文档 **5.15**。
- 测试产物一律进 **`target/test/`**（规则 7）。
- 用户程序默认以 `-O2` 编译（Hao 帧以 shadow 精确根为准；`-O2` 主要改善 os_block C 叶/无 shadow 路径的栈布局质量）。
- 语法/语义长文与 Go·Java·C# 对照：[`docs/hao语法.md`](docs/hao语法.md)。

**修改后重新构建**：
```powershell
# 改了语法 .g4 后重新生成分析器
antlrgen

# 改了运行时 runtime_*.c 后重新编译
powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1
```

---

## 四、语言与语法（摘要）

完整语法、语义、限制与 Go / Java / C# 对照见 **[`docs/hao语法.md`](docs/hao语法.md)**。反向门禁：`script\win\negcheck.ps1`（须 **28/28**）。

最小示例：

```hao
package main;
func main() {
    val name = "HaoLang";
    fmt.println("hello, " + name);
}
```

空安全与常见误用见语法文档；本仓库不再在 README 展开长教程。

---

## 五、标准库（包一览）

详文与 API：打开 `stdlib/src/<pkg>/` 源码注释，或见 [`记忆文档.md`](记忆文档.md) 第 4 章速查 / [`docs/hao语法.md`](docs/hao语法.md)。

| 包 | 一句话 |
|----|--------|
| `lang` / `lang.Bit` | 装箱包装与位工具（隐式预导入 lang） |
| `fmt` / `object` / `hash` | 打印、Object 根、散列 |
| `collections` | List/Map/Set/Queue/Stack 与树/CHM 等 |
| `exception` / `assert` / `text` / `math` / `random` | 异常、断言、文本、数学、随机 |
| `os` | 文件 / Path / 环境 / FileStream |
| `sync` / `thread` / `channel` | 互斥与原子、线程池、channel + `haoroutine` / `select` |
| `net` | TCP/UDP + Http/MVC + Html 模板 + staticFiles |
| `reflect` / `time` / `regex` / `json` / `gc` | 反射、时间、正则、JSON、GC 统计 |
| `testing` | `hao test` 用 `T` |

`fmt` / `object.*` / `lang.*` **无需** `import`。

---

## 六、包与多文件

目录即包；`import` / `localReferences` / `dependencies` 与环境变量见 **[`docs/hao命令.md` §4](docs/hao命令.md#4-包管理hao-mod--haoprojectjson)**（设计：记忆文档 **5.15**）。

```
myproj/
  main.hao          package main;  import calc;
  calc/
    calc.hao        package calc;
```

---


## 七、构建与测试

### 构建

```powershell
# 编译器（改了 C++ 后）
cmake --build build --parallel 8     # 新增 .cpp 需先重跑 cmake 配置

# 运行时库（改了 runtime_*.c 后）
powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1
```

### 测试

> 用户侧 `hao test` 规则与对标 `go test`：[`docs/hao命令.md` §5](docs/hao命令.md#5-hao-test对标-go-test)。仓库基线仍以本节 `test.sh` / `negcheck` 为准。

语言特性 / 标准库 / 系统库全部收敛进一个**多文件集成套件** `test/suite/`（`package main`，含 calc/shapes/lib 跨包子包），`test.sh` 只编译并运行它 + 语法解析。

```bash
bash script/test.sh                 # 增量：源未变直接运行已编译 suite.exe
bash script/test.sh --rebuild-all   # 强制全量重编（改编译器/运行时/stdlib 后）
powershell -ExecutionPolicy Bypass -File script\win\negcheck.ps1   # 反向：须 28/28 编译拒绝
```

套件共 **1011** 行 stdout，全部通过。改 stdlib / 语法后务必 `--rebuild-all`（增量不查 stdlib `.hao` mtime）。

**做新特性的工作流**：写单文件临时验证 → 合并进 `test/suite/` → 归档到 `test/oldcase/`。套件文件与覆盖面见目录 `test/suite/`（不必在 README 维护长表）。

**外部库测试**（需链接参数/特定 cwd，单列验证）：
```bash
output/hao.exe run test/extc/extc.hao --link test/extc/extc.c     # 手写 C 源码
output/hao.exe run test/winsys.hao                                 # @link("ws2_32") + ntohs → 13330
output/hao.exe run test/linkpath/linkpath.hao                      # @link 相对路径
output/hao.exe run test/autofind/autofind.hao                      # 默认 ./lib 发现
HAO_LDFLAGS="-lws2_32" output/hao.exe run test/sysenv.hao          # 环境变量
```

---

## 八、交叉编译（用户程序 → Linux ✅）

```powershell
# 一次性准备（下载 musl sysroot，约 110 MB）
powershell -ExecutionPolicy Bypass -File script\win\fetch_sysroot.ps1 -Target linux-amd64
powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1 -Target linux-amd64

# 之后即可交叉编译
hao build test\hello.hao --target linux-amd64 -o hello
```

产出**静态链接的 Linux ELF**，无任何动态依赖（`7f 45 4c 46` 魔数）。

**为什么需要 sysroot**：LLVM 只是编译器，不含任何平台的 C 标准库。`.hao → Linux .o` 可行，但链接成可执行文件缺 `crt1.o`/`libc.a`/`libgcc.a`，必须准备一份目标系统的 sysroot。用 **musl**（专为静态链接设计，真正零依赖；glibc 静态链接有 NSS/dlopen 问题）。

**注意区分**：
- 交叉编译**用户程序**到 Linux → ✅ 已支持。
- 交叉编译**编译器自身**到 Linux → ⬜ 需自举（Stage 10）后，用 HaoLang 交叉编译 hao。

---

## 九、目录结构

```
（仓库根）
├── src                     编译器（C++17）
│   ├── main.cpp            hao 命令入口
│   ├── ast/                HaoLangLexer.g4 / HaoLangParser.g4 / generated/
│   ├── util/               公共工具 FileUtil/PathUtil/ConsoleUtil/StringUtil
│   ├── mod/                haoproject.json 清单（HaoProject，v0.38）
│   ├── tool/               hao test / hao fmt（v0.40）
│   ├── driver/             编译驱动 Driver/Paths/Compile/Resolve/Link
│   ├── sema/               类型系统/符号表/诊断 Type/SymbolTable/Diagnostic
│   └── irgen/               IR 生成 IRGen{Class,Expr,Value,Control,Except,Lambda,Literal}.cpp
├── stdlib                  运行时（C）+ HaoLang 标准库包
│   ├── runtime_*.c         按功能拆分：gc/string/array/object/box/exception/print/panic/hash/os/sync/net
│   └── src/                .hao 标准库包：collections/exception/fmt/os/sync/net
├── script/
│   ├── setup_env.ps1       新机器入口（转发 win/setup_env.ps1）
│   ├── test.sh             测试套件（跨平台）
│   ├── haoreg_server.py    本地包仓私服
│   └── win/                Windows PowerShell 工具链
│       ├── env.ps1         会话环境（根目录 env.ps1 点源至此）
│       ├── setup_env.ps1   scoop + msvc / llvm / winlibs / sysroot
│       ├── fetch_*.ps1     msvc / llvm / winlibs / sysroot
│       ├── hao_net.ps1     代理/下载（直连→ghfast→http_proxy）
│       ├── build_runtime.ps1 / package.ps1 / negcheck.ps1
│       └── proxy.local.ps1.example
├── repo/
│   ├── LocalRepo/          测试本地仓（HAO_REPO）
│   └── RegisterRepo/       私服远程源数据（仅 haoreg）
├── docs/                   文档治理 / 坑债 / 项目时间线；hao语法.md / hao命令.md
├── haolang-example/        **用户向示例库**（hello / 特性 / 多文件 / Web / 项目 / 测试）
├── 记忆文档.md             AI 工作规则与设计决策（权威）
├── test/
│   ├── suite/              多文件集成套件（基线 1011 行；含 demo/web、corp 多包）
│   ├── oldcase/            归档旧单文件（net.hao 等可专项冒烟）
│   └── syntax.hao          语法解析覆盖（只 parse）
├── lib/                    外部依赖与工具链（LLVM/ANTLR/sysroot/win CRT）
├── target/                 发行包 + **test/** 测试临时区（规则 7）
├── output/hao.exe          编译器
└── VERSION                 版本唯一来源
```

> **`lib/` / `output/` / `target/`**：`lib/`=工具链；`output/`=本机编译器；`target/`=发行包与 `target/test/`（禁止往 target 根乱扔文件）。

### 打包发行版（Windows x64，无 VS 目标机可用）

仅宿主 **`win-amd64`** 可打包（本机 `hao.exe` + LLVM 均为 Windows x64）。`-Target` 其它平台会明确失败（交叉编译「用户程序」已支持，交叉打包「编译器自身」尚未支持）。

```powershell
# 仓库根；.ps1 须 UTF-8 BOM
. .\env.ps1
haobuild
powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1
powershell -ExecutionPolicy Bypass -File script\win\fetch_winlibs.ps1 -Target win-amd64
# 打包前：不要把终端 cwd 停在 target\...\bin（会锁目录）
powershell -ExecutionPolicy Bypass -File script\win\package.ps1 -Zip
# 调试可加 -SkipSelfCheck 跳过运行自检
```

发行版目录须完整保留：
- `bin\hao.exe`
- `lib\llvm\bin\`（clang + lld-link）
- `lib\sysroot\win-amd64\lib\`（CRT 最小集：libcmt / libvcruntime / libucrt / kernel32 / oldnames / **uuid**）
- `stdlib\`（`libhaort.a` + `src\` 下全部 `.hao` 包，含 **net**）
- `examples\`（整树来自仓库 [`haolang-example/`](haolang-example/)，含 `01-hello` 等；不含 `test/suite` / `oldcase`）
- `docs\`（`hao语法.md`、`hao命令.md`）

整目录拷到任意 Windows x64 即可用。`hao` 按自身相对路径查找工具链与标准库；**不要只拷贝 `hao.exe`**。

### 版本管理

`VERSION` 是版本号唯一来源，CMake 配置阶段注入宏 `HAO_VERSION`。改版本号后需重跑 `cmake -S . -B build`。

---

## 十、版本时间线

各版本专节见 [`docs/项目时间线/索引.md`](docs/项目时间线/索引.md)。当前合订：[`v0.50-0.59.md`](docs/项目时间线/v0.50-0.59.md)。

---

## 十一、已知限制


- GC（**v0.55.10**）：可达性主路径已交付；concurrent sweep 为停顿排期。详文 [`docs/IR与GC契约.md`](docs/IR与GC契约.md)。
- 已有真正 **`Byte`（0～255）**、**`Char`（Unicode 码点 i32）** 与紧凑数组；String = `ptr`→`HaoString{len,cap,data[]}`，`.length`/`s[i]` 按码点。
- **泛型接口已实现（v0.18.0）**：`Iterable<T>`/`Iterator<T>`；`toArray()` 仍作兜底。
- **反射**：类型自省 + 字段读写 + 注解（含 value 等参数）+ **方法 invoke**（含 `invokeFloat`）已有；运行时动态定义类/成员需 VM，后续。
- **运行时 / 发行包（v0.21.1）**：
  - 系统 API（net/thread/…）Windows 全部 dynload，不依赖 SDK 的 `ws2_32.lib` 等。
  - 无 VS 机器靠发行包内 CRT 最小集（`libcmt`+`libvcruntime`+`libucrt`+`kernel32`+`oldnames`+`uuid`）。
  - 必须保持发行包目录结构；用户自行 `@link`/`-l` 仍可能需要本机导入库。
- 自动属性 `{ get; set; }`、接口默认方法、接口继承接口只解析未实现；每包一个 `init()`。
- 泛型约束 `where T : Speaker` 未实现。
- 多文件/包是**整盘编译**（无 `.a`/增量编译）。
- **无** Int↔Integer 隐式自动装箱（需 `Integer.valueOf`）；`new Int`/`new String` 会当成内建类型失败。
- 位运算整数族 + 一元 `~`；`lang.Bit` 位模式为 **Long**；**json/regex/FileStream/Http** 已在 v0.28（无流式 JSON Reader/Writer、无 `@JSONField` 全量）。
- **net MVC + 包扫描**已在 **v0.29～0.32**；**v0.49** 补 Html 模板、返回值 JSON coerce、`staticFiles`（仍缺路径变量 `{id}` / 参数绑定 / IoC）。
- 并发关键字定名 **`haoroutine`**（不叫 goroutine）。
- Linux **编译器分发包**、darwin 随包 SDK：未支持。

**将就债**：A～E 已于 **v0.33** 清零。延期项与清单正文见 [`docs/坑债.md`](docs/坑债.md) 第二节；能力规划见 [`记忆文档.md`](记忆文档.md) 第 9～10 章。

---

## 十二、踩坑与技术债

**唯一正文**：[`docs/坑债.md`](docs/坑债.md)（现象→原因→解法 + 将就债 + 构建速查）。

本文件不再维护踩坑表副本。

---

## 十三、文档约定（给贡献者 / AI）

1. **全程中文**（记忆文档规则 1）。
2. 测试绿后立刻按 [`docs/文档治理.md`](docs/文档治理.md) 更新：记忆文档摘要、README 用户面、时间线专节、坑债。
3. 改前 `.bak`，收工删备份（规则 12）。
4. 新会话：记忆文档 **第 0～1 章** + 坑债将就债 + **第 10 章**。

---

## 下一步

| 状态 | 内容 |
|------|------|
| ✅ 已完成 | **v0.55.44** 定位十期；**v0.55.43** 定位九期；**v0.55.42** 定位八期；**v0.55.41** 定位七期；**v0.55.40** 定位六期 |
| 🔥 **下一批（默认开干）** | 扩 IROps（cast/gep/select）清 Expr 残留（记忆文档 §I） |
| 其后 | I0/I3/I4 dbg（挂 IROps）→ Sema/作用域机（后置）→ select / sweep → 自举 |

实现步骤见 [`记忆文档.md` 第 10 章](记忆文档.md)；历史见 [`docs/项目时间线/`](docs/项目时间线/索引.md)。
