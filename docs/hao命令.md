# HaoLang 命令参考（`hao`）

> **读者**：使用者与工具链维护者  
> **版本范围**：v0.38～v0.48（清单第 0～2 层；`HAO_REPO` 本地仓；`test` / `fmt`）  
> **对标**：命令行 UX 对标 **`go`**；清单格式对标 **.NET 项目文件思想**（JSON），**不是** `go.mod` 文法；HTTP 仓发现 API 对标 **Maven 仓库元数据** 的简化版。  
> **权威设计**：[`记忆文档.md`](../记忆文档.md) **5.15**；实现：`src/main.cpp`、`src/driver/`、`src/mod/`、`src/tool/`；本地私服：`script/haoreg_server.py`。
>
> **从哪进来**：快速开始 / 命令清单 → [`../README.md`](../README.md)；语法语义 → [`hao语法.md`](hao语法.md)；环境准备 → `script/setup_env.ps1` + 根目录 `. .\env.ps1`；Windows 脚本均在 `script/win/`。

---

## 1. 总览

`hao` 是 HaoLang 的统一工具入口。典型管线：

```
.hao 源码
  → ANTLR4 词法/语法
  → C++ IRGen 生成文本 LLVM IR（.ll）
  → clang + lld + libhaort.a
  → 原生可执行文件
```

| 命令 | 作用 | 对标 |
|------|------|------|
| `hao build` | 编译为可执行文件 | `go build` |
| `hao run` | 编译并立即运行 | `go run` |
| `hao emit` | 只产出 `.ll` | （无直接对应；调试用） |
| `hao parse` / `tokens` | 打印语法树 / 词法流 | 编译器诊断 |
| `hao mod` | 项目清单与依赖 | `go mod`（UX） |
| `hao test` | 编译并运行测试入口 | **部分**对标 `go test`（见 §5） |
| `hao fmt` | 空白 + 4 空格缩进 | **部分**对标 `gofmt`（不断行） |
| `hao clean` / `env` / `version` | 清理 / 环境 / 版本 | `go clean` / `go env` / `go version` |

```text
用法: hao <命令> [参数]

选项（build/run/emit 等编译类命令通用）:
  -o <file>            指定输出文件
  --target <平台>      交叉编译目标（win-amd64 / linux-amd64）
  --keep-ir            保留中间 .ll 文件
  -v, --verbose        显示执行的外部命令
  -l<name>             链接外部库（如 -lws2_32，可重复）
  -L<dir>              库搜索路径（可重复）
  --link <file>        直接链接 .lib/.a/.o/.c（可重复）
  --                   之后参数传给用户程序 main(args)
```

**通用注意事项**

1. **路径**：Windows 下 `hao run` 经 `cmd`/`system` 启动时，正斜杠路径可能静默失败；相对路径建议 `.\` 风格（见 [`坑债.md`](坑债.md)）。
2. **文件 vs 目录**：传文件 = 只编译列出的文件（`go run a.go b.go` 语义）；传目录 = 编译该目录全部 `.hao`（目录即包）。
3. **优化**：用户程序默认 **`-O2`**。GC 保守栈扫描依赖合理栈布局；勿为「调试方便」长期用 `-O0` 跑 GC 重负载。
4. **发行包**：只拷 `hao.exe` 不够；须整包 `bin/` + `stdlib/src/` + `lib/llvm/` + `lib/sysroot/...`。
5. **清单生效**：入口旁存在 `haoproject.json` 时，`build`/`run`/`test` 会加载并应用 `localReferences` 与 `dependencies`（见 §4）。

---

## 2. 编译与运行类命令

### 2.1 `hao build`

**作用**：把源码编译为可执行文件，不自动运行。

**执行方式**

1. 解析 CLI → `BuildOptions`。
2. `applyHaoProjectToOptions`：若发现清单，注入包搜索根（localRefs + 依赖 cache）。
3. 解析 import 闭包 → 生成一份 LLVM IR → clang/lld 链接 `libhaort.a` 与系统库。
4. 成功时打印「编译成功」（除非 `quiet`，如 `hao test`）。

**常用**

```powershell
hao build main.hao -o myapp.exe
hao build . -o app.exe                    # 当前目录整包
hao build test\hello.hao --target linux-amd64 -o hello
hao build app.hao -lws2_32 --link helper.c -v
```

| 选项 | 生效方式 | 说明 |
|------|----------|------|
| `-o` | CLI | 输出路径；缺省由入口名推导 |
| `--target` | CLI；清单 `project.target` 可作默认 | `win-amd64` / `linux-amd64` |
| `--keep-ir` | CLI | 保留 `.ll` |
| `-l` / `-L` / `--link` | CLI | 与源码内 `@link(...)`、环境变量 `HAO_LDFLAGS` 一并参与链接 |
| `--` | CLI | 仅影响 `run`/`test` 传参；`build` 通常无程序参数 |

**交叉编译注意**

- 目标 `linux-amd64` 前须准备 musl sysroot 与对应 `libhaort.a`：
  ```powershell
  powershell -ExecutionPolicy Bypass -File script\win\fetch_sysroot.ps1 -Target linux-amd64
  powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1 -Target linux-amd64
  ```
- 产出静态 ELF；魔数 `7f 45 4c 46`。交叉的是**用户程序**，不是编译器自身。

### 2.2 `hao run`

**作用**：编译后立即执行产物（临时或约定输出）。

**执行方式**：与 `build` 同一编译管线，成功后组装命令行并 `system` 启动；`--` 之后进入 `main(args: [String])`（**不含**程序名，对齐 Java/C#）。

```powershell
hao run test\hello.hao
hao run a.hao -- hello 世界
hao run . -v
```

**限制**：多入口/目录语义与 `build` 相同；若清单要求 `main` 字段，以驱动解析为准。目录/多文件时须能确定唯一 `main()`。

### 2.3 `hao emit`

**作用**：只生成 LLVM IR（`.ll`），不链接可执行文件。等价于内部 `emitIROnly=true` + `keepIR=true`。

**用途**：对照 IR、排查 IRGen、教学。

### 2.4 `hao parse` / `hao tokens`

| 命令 | 作用 |
|------|------|
| `parse` | 词法+语法；成功则打印语法树；失败打印诊断 |
| `tokens` | 打印默认通道词法记号流 |

二者**不**做语义分析、不生成 IR、不读依赖图。适合验证文法或定位解析错误。

### 2.5 `hao clean`

删除**当前工作目录**下与 `.hao` 同名的中间产物：`*.ll`、`*.hao-run.exe`。  
**不会**清理 `target/`、全局包缓存、发行包。避免误删用户文件，故只按后缀匹配。

### 2.6 `hao env` / `hao version`

| 命令 | 内容 |
|------|------|
| `version` | 编译器版本、宿主平台、ANTLR/LLVM 版本号 |
| `env` | 宿主三元组、clang 路径、`libhaort.a`、stdlib 源码根、Win CRT 库目录等 |

用于确认「跑的是哪份工具链」。发行包与开发树路径解析不同：开发树可读 CMake 注入的 `HAO_STDLIB_DIR` 等；便携发行包相对 `hao.exe` 查找。

---

## 3. `hao fmt`

**状态**：空白规范化 + **4 空格缩进**（按花括号深度）；**不是**完整 pretty-print（不断行/运算符旁空格重排）。

```text
hao fmt [选项] <文件或目录...>

  -w, --write     写回文件
  --check         若需格式化则退出码 1（不写；适合 CI）
  -v, --verbose   显示未改动的文件
```

**规则（当前）**

1. 先做语法闸门：解析失败则拒绝格式化该文件。
2. 去掉行尾空白。
3. CRLF → LF。
4. 保证文件以换行结尾。
5. 按词法默认通道的 `{`/`}`（含模板插值 `{`）深度重写行首缩进；行首 `}` 降一级；空行无前导空白；Tab 前导统一为若干 4 空格。

`-w` 与 `--check` **互斥**。目录会**递归**处理全部 `.hao`。

---

## 4. 包管理（`hao mod` + `haoproject.json`）

> **硬性决策**：不用 `hao.mod` / 自研 mod 文法。清单文件名固定为 **`haoproject.json`**（标准 JSON）。  
> 命令仍叫 **`hao mod`**，仅对标 `go mod` 的交互习惯。

### 4.1 设计原理（终局不变 · v0.48 Maven 模型）

| 原则 | 含义 |
|------|------|
| **源仓 ≠ 本地仓** | `HAO_REGISTRY` = 从哪拉；`HAO_REPO` = 落到哪、编译从哪读（对标 Maven remote / local） |
| 全局统一本地仓 | 包只存一份在 `HAO_REPO`；**默认不进项目目录**（无 vendor，除非将来显式 `hao mod vendor`） |
| `localReferences` ≠ 包 | 业务上对标 C# **ProjectReference**：本地互引，不下载、不进本地仓、不算版本依赖 |
| 源仓以环境/全局为主 | 项目可 **追加** `registry.additional`；`includeDefault` 控制是否保留官方默认 URL |
| stdlib 随工具链 | `stdlib/src` **永不**走 registry / 本地仓 |
| 依赖键 = 完整模块路径 | 如 `github.com/foo/bar`；源码 `import` 短名仍由包目录 / `package` 声明决定 |

**源仓库优先级（高 → 低）**——只决定「从哪下载」，不决定编译读路径：

1. 环境变量 `HAO_REGISTRY`（逗号分隔，可 http / 本地目录 / `file:`）
2. （预留）全局 `%APPDATA%/hao/config.json` —— **未读**
3. 项目 `registry.additional`
4. 内置官方 URL `https://pkg.haolang.org`（当 `registry.includeDefault != false`）

**本地仓**：`HAO_REPO`（默认 `~/.hao/repo`）。tidy：**先查本地仓** → 未命中再从源仓拉取写入本地仓 → 编译只读本地仓。

> **HTTP 协议**：`GET {reg}/{module}/versions.json` + `GET {reg}/{module}/{version}.zip`（WinHTTP）。  
> 可选 `HAO_TOKEN`（Bearer）、`HAO_PROXY`。  
> 根下 `repo/LocalRepo` / `repo/RegisterRepo` 仅为**测试夹具 / 私服数据**，语言不硬编码。

### 4.2 子命令

#### `hao mod init [模块路径]`

在**当前目录**创建 `haoproject.json`（已存在则失败）。

```powershell
hao mod init
hao mod init github.com/you/myapp
```

生成最小清单：`project.name` / `module` / `version` 等。模块路径建议用可反向域名风格的唯一路径，与 Go module path 习惯一致。

#### `hao mod tidy [项目目录]`

解析依赖图（含传递依赖）→ 查 `HAO_REPO` → 未命中则从 `HAO_REGISTRY` 拉取写入本地仓 → `haoproject.lock.json`（sha256 + `requiredBy`）。

```powershell
hao mod tidy
hao mod tidy .\myproj
```

**执行步骤（原理）**

1. 加载 `haoproject.json`（含 `exclude`）。
2. 合并**源**仓库列表（§4.1）；本地仓路径取 `HAO_REPO`。
3. BFS：根 `dependencies` → 读各包 `haopkg.json` 子依赖再入队。
4. 每模块累积约束；**先在本地仓选最高满足版**；未命中再查源仓并写入本地仓；冲突硬失败。
5. 本地 file 仓优先；否则 HTTP：`versions.json` + `{ver}.zip`。
6. 写出 lock（可含传递依赖条目）。

#### `hao mod why <模块> [项目目录]`

打印 lock 中该模块的版本与 `requiredBy` 列表。

```powershell
hao mod why example.com/demo/leafpkg
hao mod why example.com/demo/leafpkg .\myproj
```

**build 时自动 ensure**：有 `dependencies` 时若缺 lock、根依赖未入 lock、或 cache 缺失则 tidy；校验 sha256。

### 4.3 `haoproject.json` 配置项

完整字段（第 0～1 层已识别；未知字段解析时跳过并保留，便于向前兼容）：

```json
{
  "project": {
    "name": "myapp",
    "module": "github.com/you/myapp",
    "version": "0.1.0",
    "main": "main.hao",
    "target": "win-amd64",
    "output": "myapp.exe",
    "haoVersion": "0.41.0"
  },
  "localReferences": ["../common", "../shared"],
  "dependencies": {
    "example.com/demo/utilpkg": "^1.0.0"
  },
  "exclude": {
    "example.com/demo/unwanted": "*"
  },
  "replace": {
    "example.com/demo/utilpkg": "../forks/utilpkg"
  },
  "registry": {
    "additional": ["D:/hao-registry", "file:///D:/hao-registry", "http://127.0.0.1:8765"],
    "includeDefault": true
  }
}
```

| 字段 | 作用 | 生效方式 |
|------|------|----------|
| `project.name` | 人类可读项目名 | 元数据；init 写入 |
| `project.module` | 模块唯一路径 | 元数据；对标 Go module path |
| `project.version` | 本项目版本 | 元数据 |
| `project.main` | 主入口文件（相对清单目录） | build/run 解析入口时参考 |
| `project.target` | 默认目标平台 | 未在 CLI 指定 `--target` 时可用 |
| `project.output` | 默认输出名 | 未指定 `-o` 时可用 |
| `project.haoVersion` | 期望工具链版本（提示用） | 当前不强制校验 |
| `localReferences` | 本地项目搜索根（相对清单目录） | **编译期**追加到 import 搜索路径；不进 lock/cache |
| `dependencies` | `模块路径 → 精确版或 ^/~/>=/=` | tidy 选最高满足版（含传递依赖）→ cache + lock |
| `exclude` | `模块 → 约束标记`（键存在即排除） | tidy 跳过该模块边，不进 lock |
| `replace` | `模块 → 本地路径` 或 `module@约束` | tidy/build：本地路径优先于仓库；不进 cache（本地） |
| `registry.additional` | 追加仓库根（本地路径 / `file:` / `http(s):`） | 拼进仓库列表（低于 `HAO_REGISTRY`） |
| `registry.includeDefault` | 是否保留官方默认 URL | `false` 时不追加 `https://pkg.haolang.org`（本地冒烟常用） |

**最小可用（仅本地互引）**

```json
{
  "project": {
    "name": "myapp",
    "module": "github.com/you/myapp",
    "version": "0.1.0",
    "main": "main.hao"
  },
  "localReferences": ["../common"]
}
```

**带依赖（本地 file 仓）**：见仓库内 `test/modsmoke/depsapp/`。

### 4.4 `haoproject.lock.json`

由 `hao mod tidy`（或 build 自动 ensure）生成，**应提交版本库**（与 `go.sum` / 锁文件同类用途）。

```json
{
  "example.com/demo/utilpkg": {
    "version": "1.1.0",
    "registry": "http://127.0.0.1:8765",
    "sha256": "<目录树哈希>",
    "requiredBy": ["<root>"]
  }
}
```

| 字段 | 含义 |
|------|------|
| `version` | 锁定的**精确**版本（范围约束已解析） |
| `registry` | 逻辑来源（本地路径、HTTP URL、或 `replace:...`） |
| `sha256` | 缓存目录树哈希；`replace` 本地路径时可能为空 |
| `replaced` | 为 true 表示走本地 replace，不进 cache |
| `requiredBy` | 谁引入了它：`"<root>"` 或 `父模块@版本`（供 `hao mod why`） |

**校验**：build 时对 cache 重算哈希，与 lock 不一致则失败，提示重新 tidy。改依赖坐标后务必 tidy。

### 4.5 仓库布局（file / 服务端共用）

```text
<registry 根>/
  example.com/
    demo/
      utilpkg/
        1.0.0/
          haopkg.json       ← 元数据（可含 dependencies / description）
          utilpkg/          ← 包源码（package 名与 import 对应）
            utilpkg.hao
        1.1.0/
          ...
```

查找路径：`<registry>/<module>/<version>/`。`module` 中的 `/` 映射为子目录。

| 目录 | 用途 |
|------|------|
| `repo/LocalRepo/` | 测试用 **本地仓**（`HAO_REPO=repo/LocalRepo`；由 tidy 从 HTTP 写入） |
| `repo/RegisterRepo/` | **私服远程源数据**（仅 `haoreg_server.py --root`；与语言无关） |

### 4.5b HTTP 仓协议与本地私服

hao tidy 对 `http(s):` 仓库：

| 请求 | 响应 | 用途 |
|------|------|------|
| `GET {reg}/{module}/versions.json` | JSON 数组 `["1.0.0","1.1.0"]` | 范围选版 |
| `GET {reg}/{module}/{version}.zip` | zip（根内即该版本目录内容） | 下载进 cache |

本地优先：`resolveRegistries` 列表里先命中 file/`file:` 则不走网络。

**推荐测试私服**（动态列包 / 打 zip，对标 Maven 元数据浏览）：

```powershell
# 终端 1：起仓（默认扫 repo/RegisterRepo）
python script/haoreg_server.py --root repo/RegisterRepo --port 8765

# 浏览器打开 http://127.0.0.1:8765 （默认只显示第一层，点进去下钻）
# API：
#   GET /api/v1/packages                      → 树：根层 dirs + modules
#   GET /api/v1/packages/example.com          → 下钻一层
#   GET /api/v1/packages/example.com/demo/utilpkg
#   GET /api/v1/packages/example.com/demo/utilpkg/1.0.0
#   GET /api/v1/packages?flat=1               → 兼容：全量平铺

# 终端 2：项目侧（测试规范：源=HTTP，本地仓=repo/LocalRepo）
$env:HAO_REGISTRY = "http://127.0.0.1:8765"
$env:HAO_REPO = "repo/LocalRepo"
hao mod tidy test/modsmoke/depsapp
hao build test/modsmoke/depsapp
# 测完停止终端 1 的 python 私服
```

可选鉴权：`python script/haoreg_server.py --token secret`，客户端设 `$env:HAO_TOKEN = "secret"`。  
代理：`$env:HAO_PROXY = "http://proxy:8080"`（WinHTTP 命名代理）。

### 4.6 本地仓布局（`HAO_REPO`）

| 平台 | 默认路径 |
|------|----------|
| Windows | `%USERPROFILE%\.hao\repo` |
| Unix | `$HOME/.hao/repo` |
| 覆盖 | 环境变量 `HAO_REPO` |
| 测试 | 固定 `HAO_REPO=repo/LocalRepo` |

条目与远程一致：

```text
<HAO_REPO>/<module>/<version>/
```

例如：`…/example.com/demo/utilpkg/1.0.0/`。

### 4.7 环境变量一览

| 变量 | 第 1 层 | 作用 | 生效时机 |
|------|---------|------|----------|
| `HAO_REGISTRY` | ✅ | **源**仓库列表（逗号分隔；http / file）；未设且 `includeDefault` 时用官方 URL | tidy / build ensure |
| `HAO_REPO` | ✅ | **本地仓**根；tidy 落盘 + 编译读取 | tidy / build ensure |
| `HAO_LDFLAGS` | ✅（链接） | 额外传给链接器的参数串，如 `-lws2_32` | `build`/`run` 链接阶段 |
| `HAO_CFLAGS` | ✅（链接） | `--link` 编译 `.c` 时附加，如 `-I…` | 链接前编译 C |
| `HAO_PROXY` | ✅ | HTTP 命名代理 URL（WinHTTP） | tidy / HTTP 拉取 |
| `HAO_TOKEN` | ✅ | `Authorization: Bearer …` | tidy / HTTP 拉取 |
| `HAO_CONFIG_PATH` | 预留未读 | 将来全局 config.json 路径 | 后续 |

开发构建时 CMake 还注入 `HAO_LLVM_DIR` / `HAO_STDLIB_DIR` / `HAO_ROOT_DIR` / `HAO_VERSION` 等**编译期宏**，不是运行时用户环境变量；`hao env` 可观察其效果。

### 4.8 import 搜索顺序（编译期）

对 `import foo` / `import a/b`：

1. 相对**入口/当前包**的目录树（Go 风格子目录）。
2. `haoproject.json` → `localReferences` 解析后的根。
3. 已 tidy 的 **dependencies 缓存根**（或 replace 本地路径）。
4. 工具链自带 **`stdlib/src/<path>`**。

stdlib 始终最后一档「内建」，且不进 registry。

### 4.9 与 Go / NuGet / Maven 对照

| 概念 | Go | Hao（当前） | .NET / Java 近似 |
|------|-----|-------------|------------------|
| 清单文件 | `go.mod` | `haoproject.json` | `.csproj` / `pom.xml` |
| 锁文件 | `go.sum` | `haoproject.lock.json` | `packages.lock.json` / 依赖锁定 |
| 本地工程引用 | `replace` 本地 | **`localReferences`**（首选）+ `replace` | ProjectReference |
| 本地仓 | 模块缓存目录 | `HAO_REPO`（默认 `~/.hao/repo`） | NuGet global-packages / `~/.m2/repository` |
| 版本范围 | semver 选择 | **精确 + ^/~/>=** + 传递依赖（v0.45～0.48） | 各包管理器不同 |
| 标准库 | GOROOT | 随工具链 `stdlib/src` | BCL / JDK |
| 命令 | `go mod tidy` | `hao mod tidy` | `dotnet restore` |

### 4.10 注意事项与限制

1. **禁止**再引入 `hao.mod` 文法。
2. **禁止**把远程包装进项目 `vendor/`（未实现 vendor 命令）。
3. HTTP 仓须提供 `versions.json`（范围选版）与 `{version}.zip`；本地 file 优先。
4. 传递依赖 / 冲突 / `exclude` / `hao mod why` 已交付（v0.46）；冲突硬失败。
5. `dependencies` 键必须是完整 module 路径；与源码 `import` 短名是两套概念。
6. 改清单语义须升版并同步本文件 + README + 记忆文档 5.15。
7. 测试产物遵守规则 7：一律 `target/test/`，勿污染源码树。

### 4.11 冒烟夹具

| 路径 | 覆盖 |
|------|------|
| `test/modsmoke/app` + `greeter` | `localReferences` |
| `repo/LocalRepo` | 测试本地仓（`HAO_REPO`） |
| `repo/RegisterRepo` | haoreg 远程源数据根（与语言无关） |
| `test/modsmoke/depsapp` | 精确依赖 + 本地仓 |
| `test/modsmoke/depsemver` | semver `^` 选最高版 |
| `test/modsmoke/depgraph` / `depexclude` | 传递依赖 / exclude / why |
| `test/modsmoke/depshttp` | HTTP zip（`script/haoreg_server.py`） |
| `script/haoreg_server.py` | 本地仓库服务端（列包 / 版本 / zip） |

---

## 5. `hao test`（对标 `go test` · v0.42）

> 设计权威：记忆文档 **5.16**。  
> **废将就**：不再「安静跑业务 `main`」。要跑程序请用 `hao run`。

### 5.1 定位

| | `go test` | `hao test`（v0.42） |
|--|-----------|---------------------|
| 发现 | `*_test.go` + `TestXxx` | 同包 `*_test.hao` + `func TestXxx(t: testing.T)` |
| 框架 | `testing` | stdlib **`testing`**（`T` / `runCase`） |
| 普通 build | 不含 `*_test.go` | 不含 `*_test.hao` |
| 入口 | 合成 test main | 生成 `__hao_test_main.hao`；业务 `main` 在 testMode 跳过 |
| 并行 / bench / cover | 有 | **本层不做** |
| 语言基线 | — | 仍用 `script/test.sh` + `test/suite`（集成规格，不是本命令） |

### 5.2 用法

```text
hao test [路径...] [-v] [-run regexp]

  -v, --verbose   打印每个用例 PASS 与 t.log
  -run <regexp>   只跑名称匹配的用例（ECMAScript 正则，search）

无参数时：当前目录有 haoproject.json → `.`；否则若当前目录含 *_test.hao → `.`；否则报错。
无任何 TestXxx → 非 0 退出（不回退跑 main）。
```

```powershell
hao test test\modsmoke\testdemo
hao test . -v
hao test . -run TestAdd
```

### 5.3 编写测试

```hao
// add.hao
package main;
func add(a: Int, b: Int): Int { return a + b; }

// add_test.hao
package main;
import testing;
func TestAdd(t: testing.T) {
    t.eqInt(add(2, 3), 5, "2+3");
}
```

- 参数类型须为 `testing.T` 或（`import testing.*` 后）`T`。
- 失败用 `t.error` / `t.fail` / `t.check` / `t.eq*`；`t.fatal` 结束本用例（内部 throw，由 runner 接住）。
- 未捕获异常 → 该用例 FAIL，不拖垮整个二进制。
- 普通程序断言仍可用 `assert`（抛异常）；测试内推荐 `t.*`。

### 5.4 执行流程

1. 扫描路径下 `*_test.hao`，AST 收集合法 `TestXxx`。
2. 应用 `-run` 过滤；若为空则报错。
3. 在 `target/test/hao-test/` 写 `__hao_test_main.hao`（同 package，逐个 `testing.runCase`）。
4. `BuildOptions.testMode=true`：纳入测试文件 + 隐式加载 `testing`；跳过业务 `main`。
5. 编译运行；runner 打印 `--- FAIL:` / 可选 `--- PASS:`；退出码反映失败数。
6. 删除临时 exe / harness / `.ll`。

### 5.5 与 `test.sh` / `negcheck` 分工

| 工具 | 角色 |
|------|------|
| `hao test` | 项目单元测试（`TestXxx`） |
| `script/test.sh` | 语言/stdlib **集成基线**（跑 suite 的 `main`，数 stdout 行） |
| `negcheck` | 反向：非法源码须编译失败 |

### 5.6 注意事项

- 第 1 层无外部测试包 `package foo_test`。
- 不递归 `./...`；路径由你列出。
- 产物只在 `target/test/`。

---

## 6. 链接相关环境与源码注解

除包管理变量外，链接阶段还认：

| 来源 | 示例 |
|------|------|
| CLI | `-lws2_32` `-LD:\libs` `--link foo.c` |
| 源码 | `extern func ntohs(x: Int): Int = "ntohs" @link("ws2_32");` |
| 环境 | `HAO_LDFLAGS` / `HAO_CFLAGS` |

搜索默认还包含工具链 sysroot、Win CRT 最小集（`fetch_winlibs.ps1`）等。详见 README「对接 C」与记忆文档 5.12。

---

## 7. 常见工作流

```powershell
cd D:\buildLang
. .\env.ps1

# 语言仓库自测
bash script/test.sh --rebuild-all
powershell -ExecutionPolicy Bypass -File script\win\negcheck.ps1

# 用户项目
hao mod init github.com/you/app
# 编辑 haoproject.json：localReferences / dependencies …
$env:HAO_REGISTRY = "D:\hao-registry"
hao mod tidy
hao build .
hao test .
hao fmt -w .
```

---

## 8. 后续

未实现（勿在文档中当作已可用）：

- 全局 `config.json` / 完整 JSON Schema
- `hao mod vendor`
- `hao test` 第 2 层：外部测试包 / 并行 / 子测试 / bench / 覆盖率
- `hao fmt` 断行/运算符旁空格等完整 pretty-print

权威路线见记忆文档第 **10** 章与 **5.15**「后续分层」。
