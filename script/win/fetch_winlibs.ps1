# ============================================================
#  准备 Windows 链接用 CRT 最小集（架构决策，非补丁）
# ------------------------------------------------------------
#  用法:
#    script\win\fetch_winlibs.ps1                    # 默认 win-amd64
#    script\win\fetch_winlibs.ps1 -Target win-amd64
#    script\win\fetch_winlibs.ps1 -Target win-arm64
#
#  为什么需要：
#    目标为 windows-msvc 时，clang/lld 链接 libcmt（静态 CRT）。
#    libcmt.lib 自带 DEFAULTLIB：kernel32 / libvcruntime / libucrt。
#    无 VS/SDK 的机器没有这些搜索路径 → 链接失败。
#    这与「系统 API 走 dynload、不链 ws2_32.lib」是两层问题：
#      - 系统 API：runtime 已动态加载，不依赖 SDK 导入库
#      - CRT：MSVC ABI 链接期硬需求，必须有一份对应 arch 的 .lib
#
#  多架构策略（与 linux sysroot 对称）：
#    lib/sysroot/<target>/lib/   只放该 arch 的最小 CRT 集
#      win-amd64  → x64 的 libcmt + libvcruntime + libucrt + kernel32 + oldnames + uuid
#      win-arm64  → arm64 同名库（本机 VS 装了 ARM64 工具时才可采集）
#      linux-*    → 仍用 fetch_sysroot.ps1（musl，含头文件+libc）
#      darwin-*   → Apple SDK 许可限制，不能随包分发；需本机 Xcode
#
#  禁止混放不同 arch 的 .lib（曾导致 arm vs x64 machine type conflict）。
# ============================================================

param(
    [string]$Target = "win-amd64",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path

$archMap = @{
    'win-amd64' = @{ VsArch = 'x64';  KitArch = 'x64';  TripleHint = 'x86_64' }
    'win-arm64' = @{ VsArch = 'arm64'; KitArch = 'arm64'; TripleHint = 'aarch64' }
}

if (-not $archMap.ContainsKey($Target)) {
    Write-Host "不支持的目标: $Target" -ForegroundColor Red
    Write-Host "支持: $($archMap.Keys -join ', ')" -ForegroundColor Yellow
    Write-Host "linux-* 请用 script\win\fetch_sysroot.ps1；darwin-* 需本机 Apple SDK。" -ForegroundColor DarkGray
    exit 1
}

$cfg = $archMap[$Target]
$outDir = "$root\lib\sysroot\$Target\lib"
$need = @('libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'kernel32.lib', 'oldnames.lib', 'uuid.lib')

if ((Test-Path $outDir) -and -not $Force) {
    $ok = $true
    foreach ($n in $need) {
        if (-not (Test-Path "$outDir\$n")) { $ok = $false; break }
    }
    if ($ok) {
        Write-Host "已存在 $outDir ，跳过（-Force 可重采）" -ForegroundColor Green
        exit 0
    }
}

# ---- 定位本机 VS / Windows SDK（仅采集用，不写进用户环境）----
$pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
if ([string]::IsNullOrEmpty($pf86)) { $pf86 = 'C:\Program Files (x86)' }
$vswhereExe = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhereExe)) {
    Write-Host "未找到 vswhere，无法定位 MSVC 库。请安装 VS Build Tools。" -ForegroundColor Red
    exit 1
}
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$vsRoot = & "$vswhereExe" '-latest' '-products' '*' '-requires' 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' '-property' 'installationPath'
if (-not $vsRoot) {
    $vsRoot = & "$vswhereExe" '-latest' '-products' '*' '-property' 'installationPath'
}
$ErrorActionPreference = $prevEap
if (-not $vsRoot) {
    Write-Host "未找到 Visual Studio 安装。" -ForegroundColor Red
    exit 1
}
if ($vsRoot -is [array]) { $vsRoot = $vsRoot[0] }
$vsRoot = "$vsRoot".Trim()

$msvcLib = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "lib\$($cfg.VsArch)" } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1

if (-not $msvcLib) {
    Write-Host "未找到 MSVC $($cfg.VsArch) 库目录（$vsRoot）。" -ForegroundColor Red
    Write-Host "win-arm64 需要安装 VS 的 ARM64 工具集。" -ForegroundColor Yellow
    exit 1
}

$kitRoot = 'D:\Windows Kits\10\Lib'
if (-not (Test-Path $kitRoot)) { $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" }
if (-not (Test-Path $kitRoot)) { $kitRoot = "$env:ProgramFiles\Windows Kits\10\Lib" }
if (-not (Test-Path $kitRoot)) {
    Write-Host "未找到 Windows Kits\10\Lib。" -ForegroundColor Red
    exit 1
}

$kitVer = Get-ChildItem $kitRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$ucrt = Join-Path $kitVer.FullName "ucrt\$($cfg.KitArch)"
$um   = Join-Path $kitVer.FullName "um\$($cfg.KitArch)"
if (-not (Test-Path $ucrt) -or -not (Test-Path $um)) {
    Write-Host "SDK 缺少 $($cfg.KitArch) 库: $ucrt / $um" -ForegroundColor Red
    exit 1
}

Write-Host "采集 Windows CRT 最小集 -> $Target" -ForegroundColor Cyan
Write-Host "  MSVC : $msvcLib"
Write-Host "  UCRT : $ucrt"
Write-Host "  UM   : $um"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if ($Force) { Get-ChildItem $outDir -Filter *.lib -ErrorAction Ignore | Remove-Item -Force }

$sources = @{
    'libcmt.lib'       = $msvcLib
    'libvcruntime.lib' = $msvcLib
    'oldnames.lib'     = $msvcLib
    'libucrt.lib'      = $ucrt
    'kernel32.lib'     = $um
    'uuid.lib'         = $um
}

foreach ($n in $need) {
    $src = Join-Path $sources[$n] $n
    if (-not (Test-Path $src)) {
        Write-Host "缺少: $src" -ForegroundColor Red
        exit 1
    }
    Copy-Item $src "$outDir\$n" -Force
    $kb = [math]::Round((Get-Item "$outDir\$n").Length / 1KB, 1)
    Write-Host ("  OK  {0,-20} {1,10} KB" -f $n, $kb) -ForegroundColor DarkGray
}

# 机读清单，便于 package / 诊断
@{
    target    = $Target
    arch      = $cfg.VsArch
    libs      = $need
    collected = (Get-Date -Format 'o')
    msvc      = $msvcLib
    sdk       = $kitVer.FullName
} | ConvertTo-Json | Set-Content "$outDir\MANIFEST.json" -Encoding UTF8

$total = (Get-ChildItem $outDir -Filter *.lib | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("完成: {0}  ({1:N1} MB)" -f $outDir, $total) -ForegroundColor Green
