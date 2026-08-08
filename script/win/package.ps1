# ============================================================
#  打包 HaoLang 分发版到 target 目录
# ------------------------------------------------------------
#  用法:
#    script\win\package.ps1                        打包宿主平台
#    script\win\package.ps1 -Target linux-amd64    打包指定平台
#    script\win\package.ps1 -Zip                   同时生成 zip
#
#  产出: target\{os}-{arch}-haolang-{版本}\
#        例如 target\win-amd64-haolang-0.3.0\
# ============================================================

param(
    [string]$Target = "",
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path

# ---------- 读取版本号（VERSION 是唯一来源）----------
$version = (Get-Content "$root\VERSION" -Raw).Trim()

# ---------- 确定目标平台 ----------
if (-not $Target) {
    # 宿主平台：Windows x64
    $Target = if ([Environment]::Is64BitOperatingSystem) { "win-amd64" } else { "win-386" }
}

$knownTargets = @('win-amd64','win-arm64','linux-amd64','linux-arm64','darwin-amd64','darwin-arm64')
if ($Target -notin $knownTargets) {
    Write-Host "未知目标平台: $Target" -ForegroundColor Red
    Write-Host "支持: $($knownTargets -join ', ')" -ForegroundColor Yellow
    exit 1
}

$distName = "$Target-haolang-$version"
$out      = "$root\target\$distName"
$isWin    = $Target.StartsWith('win-')
$exeExt   = if ($isWin) { '.exe' } else { '' }
$hostTarget = "win-amd64"

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
    exit 1
}

Write-Host "打包 HaoLang $version -> $Target" -ForegroundColor Cyan
Write-Host "  输出目录: $out"

# ---------- 前置检查 ----------
# 运行时库名：宿主用 libhaort.a，交叉编译用 libhaort-{target}.a
$rtName = if ($Target -eq $hostTarget) { "libhaort.a" } else { "libhaort-$Target.a" }

$required = @(
    @{ Path = "$root\output\hao.exe";              Desc = "编译器（需先执行 haobuild）" }
    @{ Path = "$root\stdlib\$rtName";              Desc = "运行时库（需先执行 build_runtime.ps1 -Target $Target）" }
    @{ Path = "$root\lib\llvm\bin\clang.exe";      Desc = "LLVM clang" }
)
# Windows 目标用 lld-link（COFF），其他平台用 ld.lld（ELF）
if ($isWin) {
    $required += @{ Path = "$root\lib\llvm\bin\lld-link.exe"; Desc = "COFF 链接器" }
    # CRT 最小集：无 VS 目标机链接必需（fetch_winlibs.ps1）
    $required += @{ Path = "$root\lib\sysroot\$Target\lib\libcmt.lib"; Desc = "Win CRT 最小集（script\\fetch_winlibs.ps1 -Target $Target）" }
    $required += @{ Path = "$root\lib\sysroot\$Target\lib\kernel32.lib"; Desc = "Win CRT 最小集（kernel32.lib）" }
} else {
    $required += @{ Path = "$root\lib\llvm\bin\ld.lld.exe";   Desc = "ELF 链接器" }
}

$missing = $false
foreach ($r in $required) {
    if (-not (Test-Path $r.Path)) {
        Write-Host "  缺少: $($r.Path)" -ForegroundColor Red
        Write-Host "        $($r.Desc)" -ForegroundColor Yellow
        $missing = $true
    }
}
if ($missing) { exit 1 }

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
foreach ($d in @('bin','stdlib','stdlib\src','lib\llvm\bin','examples')) {
    New-Item -ItemType Directory -Path "$out\$d" -Force | Out-Null
}
if ($isWin) {
    New-Item -ItemType Directory -Path "$out\lib\sysroot\$Target\lib" -Force | Out-Null
}

# ---------- 拷贝 ----------
# 编译器本体（放 bin 便于加入 PATH）
Copy-Item "$root\output\hao.exe" "$out\bin\hao$exeExt"

# 运行时库：分发时统一命名为 libhaort.a，
# 因为在目标机上它就是"宿主"平台的库
Copy-Item "$root\stdlib\$rtName" "$out\stdlib\libhaort.a"
# 运行时源码（按功能拆分的模块）
Copy-Item "$root\stdlib\runtime_internal.h" "$out\stdlib\"
Get-ChildItem "$root\stdlib\runtime_*.c" | ForEach-Object {
    Copy-Item $_.FullName "$out\stdlib\"
}

# 标准库 .hao 源码包（必需！hao.exe 依赖 ../stdlib/src 定位 os/sync/
# collections/exception/fmt 等 import 包，缺了它们，目标机上这些
# 标准库全部无法使用。见 DriverResolve.cpp stdlibSrcDir()）
Get-ChildItem "$root\stdlib\src" -Directory | ForEach-Object {
    Copy-Item $_.FullName "$out\stdlib\src\" -Recurse
}

# 后端工具链
Copy-Item "$root\lib\llvm\bin\clang.exe" "$out\lib\llvm\bin\"
if ($isWin) {
    Copy-Item "$root\lib\llvm\bin\lld-link.exe" "$out\lib\llvm\bin\"
    # Windows CRT 最小集（按 arch 分目录，禁止混放）
    Copy-Item "$root\lib\sysroot\$Target\lib\*.lib" "$out\lib\sysroot\$Target\lib\"
    if (Test-Path "$root\lib\sysroot\$Target\lib\MANIFEST.json") {
        Copy-Item "$root\lib\sysroot\$Target\lib\MANIFEST.json" "$out\lib\sysroot\$Target\lib\"
    }
} else {
    Copy-Item "$root\lib\llvm\bin\ld.lld.exe" "$out\lib\llvm\bin\"
}

# 示例代码：单文件示例 + 多文件/跨包目录示例
Copy-Item "$root\test\*" "$out\examples\" -ErrorAction SilentlyContinue
foreach ($d in @('multifile','pkgdemo','pkgshapes')) {
    if (Test-Path "$root\test\$d") { Copy-Item "$root\test\$d" "$out\examples\" -Recurse }
}

# ---------- 版本清单 ----------
$stdlibPkgs = (Get-ChildItem "$root\stdlib\src" -Directory | ForEach-Object { $_.Name }) -join ', '
$manifest = [ordered]@{
    name           = 'haolang'
    version        = $version
    target         = $Target
    os             = $Target.Split('-')[0]
    arch           = $Target.Split('-')[1]
    buildTime      = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    buildHost      = "$([Environment]::OSVersion.VersionString) / $env:PROCESSOR_ARCHITECTURE"
    llvmVersion    = '22.1.8'
    antlrVersion   = '4.13.2'
    stdlibPackages = $stdlibPkgs
}
$manifest | ConvertTo-Json | Set-Content "$out\VERSION.json" -Encoding UTF8
$version | Set-Content "$out\VERSION" -Encoding UTF8 -NoNewline

# ---------- 使用说明 ----------
$readme = @"
HaoLang $version  ($Target)
$('=' * 40)

无需安装，解压即用。

快速开始
--------
  bin\hao$exeExt run   examples\hello.hao      编译并运行
  bin\hao$exeExt build examples\hello.hao      编译为可执行文件
  bin\hao$exeExt emit  examples\hello.hao      生成 LLVM IR
  bin\hao$exeExt env                           查看工具链信息
  bin\hao$exeExt version                       查看版本

把 bin 目录加入 PATH 后可直接使用 hao 命令。

目录结构
--------
  bin\            编译器 hao$exeExt
  lib\llvm\bin\   后端工具链（clang + 链接器）
  lib\sysroot\    链接用 CRT/sysroot（Windows：CRT 最小集；按目标 arch 分目录）
  stdlib\         运行时库 libhaort.a
  examples\       示例代码

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
$readme | Set-Content "$out\README.txt" -Encoding UTF8

# ---------- 自检（仅宿主平台可执行）----------
if ($Target -eq $hostTarget) {
    Write-Host "`n验证打包结果..." -ForegroundColor Cyan
    Push-Location $out
    try {
        & ".\bin\hao.exe" env |
            Select-String -Pattern '(''hao''|compiler|stdio|toolchain|version)' |
            ForEach-Object { "  " + $_.Line }

        # 1) 基础示例
        $r = & ".\bin\hao.exe" run "examples\hello.hao" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "hello 自检失败: $r" }
        Write-Host "  run hello.hao      -> OK: $r" -ForegroundColor Green

        # 2) 验证标准库包打包完整：os + sync + net（缺 stdlib/src 会「找不到包」）
        #    必须用发行包内相对路径；勿依赖本机 D:\buildLang\stdlib 绝对路径兜底
        if (-not (Test-Path ".\stdlib\src\net")) {
            throw "发行包缺少 stdlib\src\net（import net 会失败）"
        }
        $probe = "$root\script\package_selfcheck.hao"
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
        Remove-Item $probe -Force -ErrorAction Ignore

        # 3) 标准库路径必须落在发行包内（相对 bin/../stdlib），不能指向开发树。
        #    用 ASCII 路径片段匹配，避免 PS5.1 中文编码导致 -match 误失败。
        $envOut = & ".\bin\hao.exe" env 2>&1 | Out-String
        if ($envOut -notmatch 'stdlib[/\\]src') {
            throw "hao env 未显示标准库源码路径（输出中应含 stdlib/src）:`n$envOut"
        }
        # 开发树兜底特征：.../buildLang/stdlib/src 且路径中无 target\
        if ($envOut -match 'buildLang[/\\]stdlib[/\\]src' -and $envOut -notmatch 'target[/\\]') {
            throw "发行包仍回退到开发树绝对路径 D:\buildLang\stdlib（便携布局探测失败）:`n$envOut"
        }
        Write-Host "  hao env 标准库路径 -> OK" -ForegroundColor Green
    } finally {
        Pop-Location
    }
} else {
    Write-Host "`n（交叉编译包，跳过运行自检）" -ForegroundColor Yellow
}

# ---------- 打 zip ----------
if ($Zip) {
    $zipPath = "$root\target\$distName.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path $out -DestinationPath $zipPath
    $zipMB = (Get-Item $zipPath).Length / 1MB
    Write-Host ("`nzip: {0}  ({1:N1} MB)" -f $zipPath, $zipMB) -ForegroundColor Green
}

$sizeMB = (Get-ChildItem $out -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("`n打包完成: {0}  ({1:N1} MB)" -f $out, $sizeMB) -ForegroundColor Green
