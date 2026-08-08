# ============================================================
#  打包 HaoLang 分发版到 target 目录
# ------------------------------------------------------------
#  用法（仓库根）:
#    script\win\package.ps1                        打包 win-amd64
#    script\win\package.ps1 -Zip                   同时生成 zip
#    script\win\package.ps1 -SkipSelfCheck         跳过运行自检
#    script\win\package.ps1 -Target linux-amd64    （当前会失败并说明原因）
#
#  前置: haobuild → build_runtime.ps1 → fetch_winlibs.ps1
#  产出: target\win-amd64-haolang-{版本}\
#        例如 target\win-amd64-haolang-0.48.0\
# ============================================================

param(
    [string]$Target = "",
    [switch]$Zip,
    [switch]$SkipSelfCheck
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path

# ---------- 读取版本号（VERSION 是唯一来源）----------
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()

# ---------- 确定目标平台 ----------
# 本脚本仅能产出 Windows x64 宿主分发包（本机 hao.exe + LLVM 均为 win-amd64）
$hostTarget = 'win-amd64'
if (-not $Target) { $Target = $hostTarget }

$knownTargets = @('win-amd64','win-arm64','linux-amd64','linux-arm64','darwin-amd64','darwin-arm64')
if ($Target -notin $knownTargets) {
    Write-Host "未知目标平台: $Target" -ForegroundColor Red
    Write-Host "支持: $($knownTargets -join ', ')" -ForegroundColor Yellow
    Write-Host "当前可打包: $hostTarget" -ForegroundColor Yellow
    exit 1
}

$distName = "$Target-haolang-$version"
$out      = Join-Path $root "target\$distName"
$exeExt   = '.exe'

# ------------------------------------------------------------
#  非宿主平台的分发包：当前无法产出
# ------------------------------------------------------------
#  分发包里的 hao 编译器本体与 clang/lld 后端都必须是目标平台的
#  原生可执行文件。而本机只有 Windows 版的 hao.exe 与 LLVM 工具，
#  把它们改名放进 linux 包里只会得到一个跑不起来的包。
#
#  要真正产出 Linux 分发包，需要满足其一：
#    1) 用 HaoLang 自举后，交叉编译出 Linux 版 hao（Stage 10 之后）
#    2) 下载 Linux 版 LLVM 工具链，并在 Linux 上构建 hao
#    3) 在 Linux 机器 / WSL / CI 上执行本脚本
#
#  注意区分两件事：
#    - 交叉编译"用户程序"到 Linux —— 已经支持，见 hao build --target
#    - 交叉编译"编译器自身"到 Linux —— 即本包，尚未支持
if ($Target -ne $hostTarget) {
    Write-Host ""
    Write-Host "无法产出 $Target 分发包。" -ForegroundColor Red
    Write-Host ""
    Write-Host "原因：分发包需包含目标平台的原生 hao 与 LLVM 后端，" -ForegroundColor Yellow
    Write-Host "      而本机只有 Windows 版二进制。" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "已支持的是把「用户程序」交叉编译到 $Target：" -ForegroundColor Cyan
    Write-Host "      hao build test\hello.hao --target $Target"
    Write-Host ""
    Write-Host "要产出 $Target 分发包，需要：" -ForegroundColor Cyan
    Write-Host "      - 完成自举后交叉编译 hao 自身（Stage 10），或"
    Write-Host "      - 在 $Target 平台 / WSL / CI 上执行本脚本"
    Write-Host ""
    Write-Host "当前可打包: $hostTarget" -ForegroundColor Cyan
    Write-Host ""
    exit 1
}

Write-Host "打包 HaoLang $version -> $Target" -ForegroundColor Cyan
Write-Host "  输出目录: $out"

# ---------- 前置检查 ----------
$rtName = 'libhaort.a'
$crtLibs = @('libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'kernel32.lib', 'oldnames.lib', 'uuid.lib')
$sysrootLib = Join-Path $root "lib\sysroot\$Target\lib"

$required = @(
    @{ Path = (Join-Path $root 'output\hao.exe');              Desc = "编译器（需先执行 haobuild）" }
    @{ Path = (Join-Path $root "stdlib\$rtName");              Desc = "运行时库（需先执行 script\win\build_runtime.ps1）" }
    @{ Path = (Join-Path $root 'lib\llvm\bin\clang.exe');      Desc = "LLVM clang（需先执行 script\win\fetch_llvm.ps1 或 setup_env）" }
    @{ Path = (Join-Path $root 'lib\llvm\bin\lld-link.exe');   Desc = "COFF 链接器 lld-link" }
)
foreach ($lib in $crtLibs) {
    $required += @{
        Path = (Join-Path $sysrootLib $lib)
        Desc = "Win CRT 最小集（script\win\fetch_winlibs.ps1 -Target $Target）"
    }
}

$missing = $false
foreach ($r in $required) {
    if (-not (Test-Path $r.Path)) {
        Write-Host "  缺少: $($r.Path)" -ForegroundColor Red
        Write-Host "        $($r.Desc)" -ForegroundColor Yellow
        $missing = $true
    }
}
if ($missing) {
    Write-Host ""
    Write-Host "打包配方（仓库根）:" -ForegroundColor Cyan
    Write-Host "  . .\env.ps1"
    Write-Host "  haobuild"
    Write-Host "  powershell -ExecutionPolicy Bypass -File script\win\build_runtime.ps1"
    Write-Host "  powershell -ExecutionPolicy Bypass -File script\win\fetch_winlibs.ps1 -Target win-amd64"
    Write-Host "  powershell -ExecutionPolicy Bypass -File script\win\package.ps1 -Zip"
    Write-Host ""
    exit 1
}

# ---------- 建立目录 ----------
if (Test-Path $out) {
    try {
        Remove-Item $out -Recurse -Force -ErrorAction Stop
    } catch {
        $bak = "$out.old_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
        Write-Host "  旧目录被占用，尝试改名为: $bak" -ForegroundColor Yellow
        try {
            Move-Item -LiteralPath $out -Destination $bak -Force -ErrorAction Stop
        } catch {
            # 仍占用（常见原因：终端 cwd 停在 bin\）→ 换新目录名继续打包
            $out = "$out-fresh"
            $distName = Split-Path $out -Leaf
            Write-Host "  改用输出目录: $out" -ForegroundColor Yellow
            Write-Host "  提示: 请把占用旧目录的终端 cd 到别处后再删旧包。" -ForegroundColor DarkGray
        }
    }
}
if (Test-Path $out) { Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue }
foreach ($d in @('bin','stdlib','stdlib\src','lib\llvm\bin','examples','docs')) {
    New-Item -ItemType Directory -Path (Join-Path $out $d) -Force | Out-Null
}
New-Item -ItemType Directory -Path (Join-Path $out "lib\sysroot\$Target\lib") -Force | Out-Null

# ---------- 拷贝 ----------
# 编译器本体（放 bin 便于加入 PATH）
Copy-Item (Join-Path $root 'output\hao.exe') (Join-Path $out "bin\hao$exeExt")

# 运行时库：分发时统一命名为 libhaort.a，
# 因为在目标机上它就是"宿主"平台的库
Copy-Item (Join-Path $root "stdlib\$rtName") (Join-Path $out 'stdlib\libhaort.a')
# 运行时源码（按功能拆分的模块）
Copy-Item (Join-Path $root 'stdlib\runtime_internal.h') (Join-Path $out 'stdlib\')
Get-ChildItem (Join-Path $root 'stdlib') -Filter 'runtime_*.c' | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $out 'stdlib\')
}

# 标准库 .hao 源码包（必需！hao.exe 依赖 ../stdlib/src 定位 os/sync/
# collections/exception/fmt 等 import 包，缺了它们，目标机上这些
# 标准库全部无法使用。见 DriverResolve.cpp stdlibSrcDir()）
Get-ChildItem (Join-Path $root 'stdlib\src') -Directory | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $out 'stdlib\src\') -Recurse
}

# 后端工具链（win-amd64：clang + lld-link）
Copy-Item (Join-Path $root 'lib\llvm\bin\clang.exe') (Join-Path $out 'lib\llvm\bin\')
Copy-Item (Join-Path $root 'lib\llvm\bin\lld-link.exe') (Join-Path $out 'lib\llvm\bin\')
# Windows CRT 最小集（按 arch 分目录，禁止混放）
Copy-Item (Join-Path $sysrootLib '*.lib') (Join-Path $out "lib\sysroot\$Target\lib\")
$manifestSrc = Join-Path $sysrootLib 'MANIFEST.json'
if (Test-Path $manifestSrc) {
    Copy-Item $manifestSrc (Join-Path $out "lib\sysroot\$Target\lib\")
}

# 示例库：整树拷贝 haolang-example/ → examples/（勿用 test/suite、oldcase）
$exampleRoot = Join-Path $root 'haolang-example'
$helloEx = Join-Path $exampleRoot '01-hello\hello.hao'
if (-not (Test-Path $helloEx)) {
    Write-Host "  缺少必需示例: $helloEx" -ForegroundColor Red
    Write-Host "        请维护仓库根 haolang-example\01-hello\hello.hao" -ForegroundColor Yellow
    exit 1
}
Copy-Item (Join-Path $exampleRoot '*') (Join-Path $out 'examples\') -Recurse -Force

# 用户文档：语法与命令手册
foreach ($doc in @('hao语法.md', 'hao命令.md')) {
    $srcDoc = Join-Path $root "docs\$doc"
    if (-not (Test-Path $srcDoc)) {
        Write-Host "  缺少文档: $srcDoc" -ForegroundColor Red
        exit 1
    }
    Copy-Item $srcDoc (Join-Path $out 'docs\')
}

# ---------- 版本清单 ----------
$llvmVersion = '22.1.8'
try {
    $clangVerOut = & (Join-Path $root 'lib\llvm\bin\clang.exe') --version 2>&1 | Out-String
    if ($clangVerOut -match 'clang version\s+(\d+\.\d+\.\d+)') {
        $llvmVersion = $Matches[1]
    }
} catch {
    # 保留回退字面量
}

$stdlibPkgs = (Get-ChildItem (Join-Path $root 'stdlib\src') -Directory | ForEach-Object { $_.Name }) -join ', '
$manifest = [ordered]@{
    name           = 'haolang'
    version        = $version
    target         = $Target
    os             = $Target.Split('-')[0]
    arch           = $Target.Split('-')[1]
    buildTime      = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    buildHost      = "$([Environment]::OSVersion.VersionString) / $env:PROCESSOR_ARCHITECTURE"
    llvmVersion    = $llvmVersion
    antlrVersion   = '4.13.2'
    stdlibPackages = $stdlibPkgs
}
$manifest | ConvertTo-Json | Set-Content (Join-Path $out 'VERSION.json') -Encoding UTF8
$version | Set-Content (Join-Path $out 'VERSION') -Encoding UTF8 -NoNewline

# ---------- 使用说明 ----------
$readme = @"
HaoLang $version  ($Target)
$('=' * 40)

无需安装，解压即用。

快速开始
--------
  bin\hao$exeExt run   examples\01-hello\hello.hao   编译并运行
  bin\hao$exeExt build examples\01-hello\hello.hao   编译为可执行文件
  bin\hao$exeExt emit  examples\01-hello\hello.hao   生成 LLVM IR
  bin\hao$exeExt env                                 查看工具链信息
  bin\hao$exeExt version                             查看版本

更多示例见 examples\README.md；语法/命令见 docs\hao语法.md、docs\hao命令.md。

把 bin 目录加入 PATH 后可直接使用 hao 命令。

目录结构
--------
  bin\            编译器 hao$exeExt
  lib\llvm\bin\   后端工具链（clang + 链接器）
  lib\sysroot\    链接用 CRT/sysroot（Windows：CRT 最小集；按目标 arch 分目录）
  stdlib\         运行时库 libhaort.a
  examples\       示例库（来自仓库 haolang-example/）
  docs\           hao语法.md / hao命令.md

注意
----
请保持目录结构不变。hao 按自身相对位置查找
lib\llvm\bin\clang、stdlib\libhaort.a，以及
lib\sysroot\<平台>\lib（Windows 无 VS 时链接必需）。

生成的程序为原生机器码、静态链接，
在目标平台上可独立分发，无需任何运行时。

源文件编码统一使用 UTF-8（支持带或不带 BOM）。
控制台输出会自动切换到 UTF-8 代码页，
中文与 Emoji 均可正确显示。
"@
$readme | Set-Content (Join-Path $out 'README.txt') -Encoding UTF8

# ---------- 自检（仅宿主平台可执行）----------
if ($SkipSelfCheck) {
    Write-Host "`n（-SkipSelfCheck：跳过运行自检）" -ForegroundColor Yellow
} else {
    Write-Host "`n验证打包结果..." -ForegroundColor Cyan
    $probeDir = Join-Path $root 'target\test\package_selfcheck'
    $probe = Join-Path $probeDir 'probe.hao'
    New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
    Push-Location $out
    try {
        & ".\bin\hao.exe" env |
            Select-String -Pattern '(''hao''|compiler|stdio|toolchain|version)' |
            ForEach-Object { "  " + $_.Line }

        # 1) 基础示例（来自 haolang-example/01-hello）
        $r = & ".\bin\hao.exe" run "examples\01-hello\hello.hao" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "hello 自检失败: $r" }
        Write-Host "  run 01-hello      -> OK: $r" -ForegroundColor Green
        foreach ($doc in @('docs\hao语法.md', 'docs\hao命令.md')) {
            if (-not (Test-Path ".\$doc")) { throw "发行包缺少 $doc" }
        }
        Write-Host "  docs 手册         -> OK" -ForegroundColor Green

        # 2) 验证标准库包打包完整：os + sync + net（缺 stdlib/src 会「找不到包」）
        #    必须用发行包内相对路径；勿依赖开发树 stdlib 绝对路径兜底
        if (-not (Test-Path ".\stdlib\src\net")) {
            throw "发行包缺少 stdlib\src\net（import net 会失败）"
        }
        @"
package main;
import os;
import sync;
import net;
func main() {
    os.writeFile("pkg_selfcheck.tmp", "ok");
    val s = os.readFile("pkg_selfcheck.tmp") ?? "fail";
    fmt.println("os=" + s);
    os.remove("pkg_selfcheck.tmp");
    var c: Long? = null; c = 0;
    fmt.println("sync=" + sync.atomicAdd(c, 40));
    fmt.println("net-ok");
}
"@ | Set-Content $probe -Encoding UTF8
        $r2 = & ".\bin\hao.exe" run $probe 2>&1
        if ($LASTEXITCODE -ne 0) { throw "标准库自检失败: $r2" }
        Write-Host "  run stdlib 自检    -> OK: $r2" -ForegroundColor Green

        # 3) 标准库路径必须落在发行包内（相对 bin/../stdlib），不能指向开发树。
        #    用 ASCII 路径片段匹配，避免 PS5.1 中文编码导致 -match 误失败。
        $envOut = & ".\bin\hao.exe" env 2>&1 | Out-String
        if ($envOut -notmatch 'stdlib[/\\]src') {
            throw "hao env 未显示标准库源码路径（输出中应含 stdlib/src）:`n$envOut"
        }
        # 开发树兜底特征：.../stdlib/src 落在仓库根且路径中无 target\
        if ($envOut -match 'buildLang[/\\]stdlib[/\\]src' -and $envOut -notmatch 'target[/\\]') {
            throw "发行包仍回退到开发树绝对路径（便携布局探测失败）:`n$envOut"
        }
        Write-Host "  hao env 标准库路径 -> OK" -ForegroundColor Green
    } finally {
        Pop-Location
        Remove-Item $probeDir -Recurse -Force -ErrorAction Ignore
    }
}

# ---------- 打 zip ----------
if ($Zip) {
    $zipPath = Join-Path $root "target\$distName.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path $out -DestinationPath $zipPath
    $zipMB = (Get-Item $zipPath).Length / 1MB
    Write-Host ("`nzip: {0}  ({1:N1} MB)" -f $zipPath, $zipMB) -ForegroundColor Green
}

$sizeMB = (Get-ChildItem $out -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("`n打包完成: {0}  ({1:N1} MB)" -f $out, $sizeMB) -ForegroundColor Green
