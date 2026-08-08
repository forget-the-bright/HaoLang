# ============================================================
#  下载官方 LLVM/Clang 到 lib/llvm（不进 git，靠本脚本获取）
# ------------------------------------------------------------
#  用法:
#    script\win\fetch_llvm.ps1
#    script\win\fetch_llvm.ps1 -Version 22.1.8 -Force
#
#  来源:
#    https://github.com/llvm/llvm-project/releases
#    包名: clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz
# ============================================================

param(
    [string]$Version = '22.1.8',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
. (Join-Path $PSScriptRoot 'hao_net.ps1')
Import-HaoProxyLocal -Root $root

$dest = Join-Path $root 'lib\llvm'
$clang = Join-Path $dest 'bin\clang.exe'
$lld   = Join-Path $dest 'bin\lld-link.exe'

if ((Test-Path $clang) -and (Test-Path $lld) -and -not $Force) {
    Write-Host "LLVM 已存在: $dest" -ForegroundColor Green
    Write-Host '（使用 -Force 可强制重新下载）' -ForegroundColor DarkGray
    exit 0
}

if (-not (Test-HaoNetwork -Url 'https://github.com')) {
    Write-Host '[网络] GitHub 直连 / ghfast / HTTP 代理均不可用，无法下载 LLVM。' -ForegroundColor Red
    Write-Host '请检查 https://ghfast.top ，或配置 script\win\proxy.local.ps1 / HAO_HTTP_PROXY' -ForegroundColor Yellow
    exit 1
}

$archName = 'x86_64-pc-windows-msvc'
$pkgName  = "clang+llvm-$Version-$archName.tar.xz"
$url      = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$Version/$pkgName"
$cacheDir = Join-Path $root 'lib\_cache'
$archive  = Join-Path $cacheDir $pkgName

New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null

Write-Host "下载 LLVM $Version ..." -ForegroundColor Cyan
Write-Host "  $url" -ForegroundColor DarkGray
if ((Test-Path $archive) -and -not $Force) {
    Write-Host "使用缓存: $archive" -ForegroundColor DarkGray
} else {
    Invoke-HaoDownload -Url $url -OutFile $archive
}

$seven = Get-Command 7z -ErrorAction SilentlyContinue
if (-not $seven) {
    Write-Host '需要 7z 解压 .tar.xz。请先: scoop install 7zip 或运行 script\setup_env.ps1' -ForegroundColor Red
    exit 1
}

$extractRoot = Join-Path $cacheDir "llvm-$Version-extract"
if (Test-Path $extractRoot) { Remove-Item $extractRoot -Recurse -Force }
New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null

Write-Host '解压（约数分钟）...' -ForegroundColor Cyan
$tarName = [System.IO.Path]::GetFileNameWithoutExtension($pkgName)  # *.tar
$tarPath = Join-Path $cacheDir $tarName
if (Test-Path $tarPath) { Remove-Item $tarPath -Force }
& 7z x -y $archive "-o$cacheDir" | Out-Null
if (-not (Test-Path $tarPath)) { throw "解压 xz 后未找到 $tarPath" }
& 7z x -y $tarPath "-o$extractRoot" | Out-Null

$inner = Get-ChildItem $extractRoot -Directory | Select-Object -First 1
if (-not $inner) { throw "解压后未找到 clang+llvm 目录" }
$probe = Join-Path $inner.FullName 'bin\clang.exe'
if (-not (Test-Path $probe)) { throw "解压内容不完整，缺少 bin\clang.exe" }

Write-Host "安装到 $dest ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Path (Join-Path $root 'lib') -Force | Out-Null
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
Move-Item $inner.FullName $dest

Remove-Item $tarPath -Force -ErrorAction Ignore
Remove-Item $extractRoot -Recurse -Force -ErrorAction Ignore

if (-not (Test-Path (Join-Path $dest 'bin\clang.exe'))) { throw '安装失败：缺少 clang.exe' }
if (-not (Test-Path (Join-Path $dest 'bin\lld-link.exe'))) { throw '安装失败：缺少 lld-link.exe' }

$verLine = & (Join-Path $dest 'bin\clang.exe') --version 2>&1 | Select-Object -First 1
Write-Host "LLVM 就绪: $dest" -ForegroundColor Green
Write-Host "  $verLine" -ForegroundColor DarkGray