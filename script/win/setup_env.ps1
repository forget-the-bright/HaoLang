# ============================================================
#  HaoLang 通用开发环境准备（面向 GitHub clone 后首次使用）
# ------------------------------------------------------------
#  用法:
#    powershell -ExecutionPolicy Bypass -File script\setup_env.ps1
#    powershell -ExecutionPolicy Bypass -File script\setup_env.ps1 -SkipLinux
#
#  做的事:
#    scoop（没有则装）→ cmake/ninja/python/7zip/aria2/busybox/git/java
#    → fetch_msvc（无则装 VS Build Tools）→ fetch_llvm → fetch_winlibs → fetch_sysroot
#  然后:  . .\env.ps1
# ============================================================

param(
    [switch]$SkipLinux,
    [switch]$SkipMsvc,
    [switch]$ForceLlvm
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
Set-Location $root

. (Join-Path $PSScriptRoot 'hao_net.ps1')
Import-HaoProxyLocal -Root $root

Write-Host ''
Write-Host '===== HaoLang setup_env =====' -ForegroundColor Cyan
Write-Host "根目录: $root"

# PS5.1 + scoop：收窄 PSModulePath，避免 PS7 模块污染
if ($PSVersionTable.PSVersion.Major -eq 5) {
    $sysMods = Join-Path $env:WINDIR 'system32\WindowsPowerShell\v1.0\Modules'
    $scoopMods = Join-Path $env:USERPROFILE 'scoop\modules'
    $parts = @($sysMods)
    if (Test-Path $scoopMods) { $parts += $scoopMods }
    $env:PSModulePath = ($parts -join ';')
}

# ---------- scoop ----------
if (-not (Ensure-HaoScoop -Root $root)) { exit 1 }

# 刷新 shims
$shim = Join-Path $env:USERPROFILE 'scoop\shims'
if ((Test-Path $shim) -and ($env:PATH -notlike "*$shim*")) {
    $env:PATH = "$shim;$env:PATH"
}

# ---------- 网络探测（下载依赖前）----------
if (-not (Test-HaoNetwork -Url 'https://github.com')) {
    Write-Host '[网络] GitHub 直连 / ghfast(https://ghfast.top) / HTTP 代理均不可用。' -ForegroundColor Red
    Write-Host '请配置 HTTP 代理后重试：' -ForegroundColor Yellow
    Write-Host '  copy script\win\proxy.local.ps1.example script\win\proxy.local.ps1' -ForegroundColor Yellow
    Write-Host '  编辑 proxy.local.ps1 填入本机代理，或设置 $env:HAO_HTTP_PROXY' -ForegroundColor Yellow
    exit 1
}
Write-Host '[网络] GitHub 可达（已按 直连→ghfast→http_proxy 探测）' -ForegroundColor Green

# ---------- scoop 包 ----------
$pkgs = @(
    @{ Name = 'cmake';   Cmd = 'cmake' }
    @{ Name = 'ninja';   Cmd = 'ninja' }
    @{ Name = 'python';  Cmd = 'python' }
    @{ Name = '7zip';   Cmd = '7z' }
    @{ Name = 'aria2';   Cmd = 'aria2c' }
    @{ Name = 'busybox'; Cmd = 'busybox' }
    @{ Name = 'git';     Cmd = 'git' }
)
foreach ($p in $pkgs) {
    if (-not (Get-Command $p.Cmd -ErrorAction SilentlyContinue)) {
        if (-not (Install-HaoScoopPackage -Name $p.Name -CheckCommand $p.Cmd)) {
            Write-Host "缺少 $($p.Name)，setup 中止" -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host ("  {0,-8} 已有" -f $p.Cmd) -ForegroundColor DarkGray
    }
}

# java：PATH / JAVA_HOME 优先，否则 scoop
function Test-HaoJava {
    $j = Get-Command java -ErrorAction SilentlyContinue
    if ($j) { return $true }
    if ($env:JAVA_HOME) {
        $cand = Join-Path $env:JAVA_HOME 'bin\java.exe'
        if (Test-Path $cand) {
            $env:PATH = "$(Join-Path $env:JAVA_HOME 'bin');$env:PATH"
            return $true
        }
    }
    return $false
}
if (-not (Test-HaoJava)) {
    Write-Host '[java] 环境变量未找到，scoop 安装 temurin-lts-jdk ...' -ForegroundColor Cyan
    if (-not (Install-HaoScoopPackage -Name 'temurin-lts-jdk' -CheckCommand 'java')) {
        Write-Host '[java] 安装失败：antlrgen 需要 java。可手动装 JDK 17+ 并设 JAVA_HOME' -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host '  java     已有' -ForegroundColor DarkGray
}

# python 可执行名兼容
if (-not (Get-Command python -ErrorAction SilentlyContinue) -and (Get-Command python3 -ErrorAction SilentlyContinue)) {
    Write-Host '  python   使用 python3' -ForegroundColor DarkGray
}

# ---------- MSVC（编 hao.exe / 采 CRT；STL 来自 VS，不是 LLVM）----------
if (-not $SkipMsvc) {
    $fetchMsvc = Join-Path $PSScriptRoot 'fetch_msvc.ps1'
    Write-Host '[MSVC] 检查 / 必要时安装 VS Build Tools…' -ForegroundColor Cyan
    & $fetchMsvc
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[MSVC] 未就绪。可手动装 Build Tools 后重跑，或 -SkipMsvc 跳过（随后 haobuild/winlibs 会失败）。' -ForegroundColor Yellow
    }
} else {
    Write-Host '[MSVC] 已跳过（-SkipMsvc）' -ForegroundColor DarkGray
}

# ---------- lib 依赖 ----------
$fetchLlvm = Join-Path $PSScriptRoot 'fetch_llvm.ps1'
$llvmArgs = @()
if ($ForceLlvm) { $llvmArgs += '-Force' }
& $fetchLlvm @llvmArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$winlibs = Join-Path $PSScriptRoot 'fetch_winlibs.ps1'
Write-Host '[winlibs] 检查 Windows CRT 最小集...' -ForegroundColor Cyan
& $winlibs -Target win-amd64
if ($LASTEXITCODE -ne 0) {
    Write-Host '[winlibs] 失败（通常因无 VS）。发行链接需要 CRT；请装 VS Build Tools 后重跑。' -ForegroundColor Yellow
}

if (-not $SkipLinux) {
    $sysroot = Join-Path $PSScriptRoot 'fetch_sysroot.ps1'
    Write-Host '[sysroot] 检查 linux-amd64 musl...' -ForegroundColor Cyan
    & $sysroot -Target linux-amd64
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[sysroot] 失败。可稍后重试或加 -SkipLinux 跳过。' -ForegroundColor Yellow
    }
}

# ---------- 加载会话环境并自检 ----------
. (Join-Path $PSScriptRoot 'env.ps1')
Test-HaoEnv

Write-Host '下一步:' -ForegroundColor Cyan
Write-Host '  . .\env.ps1'
Write-Host '  haobuild'
Write-Host '  powershell -File script\win\build_runtime.ps1'
Write-Host ''