# ============================================================
#  检测 / 安装 MSVC（VS Build Tools · VCTools 工作负载）
# ------------------------------------------------------------
#  用法:
#    script\win\fetch_msvc.ps1              # 已有则跳过；没有则下载安装
#    script\win\fetch_msvc.ps1 -Force       # 仍跑安装器（可修组件）
#
#  说明:
#    clang 编 hao.exe 需要本机 MSVC STL + Windows SDK（不是 LLVM 自带）。
#    安装包来自微软官方 bootstrapper，体积大（数 GB），通常需管理员。
#  来源:
#    https://aka.ms/vs/17/release/vs_BuildTools.exe
# ============================================================

param(
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
. (Join-Path $PSScriptRoot 'hao_net.ps1')
Import-HaoProxyLocal -Root $root

function Get-HaoMsvcInfo {
    $pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
    if ([string]::IsNullOrEmpty($pf86)) { $pf86 = 'C:\Program Files (x86)' }
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return @{ Ok = $false; Root = $null; MsvcInclude = $null; Vswhere = $vswhere }
    }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $vsRoot) {
        $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null
    }
    $ErrorActionPreference = $prev
    if (-not $vsRoot) {
        return @{ Ok = $false; Root = $null; MsvcInclude = $null; Vswhere = $vswhere }
    }
    $msvcDir = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    $inc = if ($msvcDir) { Join-Path $msvcDir.FullName 'include\vector' } else { $null }
    $ok = [bool]($inc -and (Test-Path -LiteralPath $inc))
    # 再确认有 VC Tools 组件（vswhere -requires）
    $ErrorActionPreference = 'Continue'
    $withVc = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    $ErrorActionPreference = $prev
    if (-not $withVc) { $ok = $false }
    return @{
        Ok          = $ok
        Root        = "$vsRoot"
        MsvcInclude = if ($msvcDir) { Join-Path $msvcDir.FullName 'include' } else { $null }
        Vswhere     = $vswhere
    }
}

function Test-HaoIsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

$info = Get-HaoMsvcInfo
if ($info.Ok -and -not $Force) {
    Write-Host "MSVC 已就绪: $($info.Root)" -ForegroundColor Green
    if ($info.MsvcInclude) {
        Write-Host "  STL: $($info.MsvcInclude)" -ForegroundColor DarkGray
    }
    exit 0
}

Write-Host '[MSVC] 未检测到 VC Tools（或 -Force），准备安装 VS Build Tools…' -ForegroundColor Cyan
Write-Host '       工作负载: Microsoft.VisualStudio.Workload.VCTools + includeRecommended' -ForegroundColor DarkGray
Write-Host '       体积较大（数 GB），需较长时间；通常需要管理员权限。' -ForegroundColor Yellow

# 微软 CDN；代理走 HAO_HTTP_PROXY
if (-not (Test-HaoNetwork -Url 'https://aka.ms')) {
    if (-not (Test-HaoNetwork -Url 'https://download.visualstudio.microsoft.com')) {
        Write-Host '[网络] 无法访问微软下载源。请配置 proxy.local.ps1 / HAO_HTTP_PROXY 后重试。' -ForegroundColor Red
        exit 1
    }
}

$cacheDir = Join-Path $root 'lib\_cache'
New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
$bootstrap = Join-Path $cacheDir 'vs_BuildTools.exe'
$url = 'https://aka.ms/vs/17/release/vs_BuildTools.exe'

Write-Host "[MSVC] 下载 bootstrapper…" -ForegroundColor Cyan
Write-Host "  $url" -ForegroundColor DarkGray
Invoke-HaoDownload -Url $url -OutFile $bootstrap

# --passive：显示进度；--quiet：全静默（CI）
$ui = if ($Quiet) { '--quiet' } else { '--passive' }
$argList = @(
    $ui, '--wait', '--norestart',
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--includeRecommended'
)

if (-not (Test-HaoIsAdmin)) {
    Write-Host '[MSVC] 当前非管理员，将弹出 UAC 提升权限…' -ForegroundColor Yellow
    $p = Start-Process -FilePath $bootstrap -ArgumentList $argList -Verb RunAs -Wait -PassThru
    $code = $p.ExitCode
} else {
    $p = Start-Process -FilePath $bootstrap -ArgumentList $argList -Wait -PassThru
    $code = $p.ExitCode
}

# VS 安装器：0 成功；3010 成功但需重启
if ($code -ne 0 -and $code -ne 3010) {
    Write-Host "[MSVC] 安装失败，退出码 $code" -ForegroundColor Red
    Write-Host '可手动安装：https://visualstudio.microsoft.com/visual-cpp-build-tools/' -ForegroundColor Yellow
    Write-Host '勾选「使用 C++ 的桌面开发」或 C++ build tools。' -ForegroundColor Yellow
    exit 1
}
if ($code -eq 3010) {
    Write-Host '[MSVC] 安装完成，但建议重启后再编 hao.exe / fetch_winlibs。' -ForegroundColor Yellow
}

$info2 = Get-HaoMsvcInfo
if (-not $info2.Ok) {
    Write-Host '[MSVC] 安装器已返回成功，但 vswhere 仍未看到 VC Tools。' -ForegroundColor Yellow
    Write-Host '       若刚装完，请新开终端或重启后再跑 setup_env / haoenv。' -ForegroundColor Yellow
    exit 1
}

Write-Host "MSVC 就绪: $($info2.Root)" -ForegroundColor Green
if ($info2.MsvcInclude) {
    Write-Host "  STL: $($info2.MsvcInclude)" -ForegroundColor DarkGray
}
exit 0