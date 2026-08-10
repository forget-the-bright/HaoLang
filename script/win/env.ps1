# ============================================================
#  HaoLang 开发环境一键加载（仅当前会话）
#  用法：. .\env.ps1
#  首次机器：先跑 script\setup_env.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$Global:HAO_ROOT = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
if (-not $Global:HAO_ROOT) {
    Write-Host '[env] 无法确定项目根，请在仓库根目录执行 . .\env.ps1' -ForegroundColor Red
    return
}

$Global:HAO_LLVM  = Join-Path $HAO_ROOT 'lib\llvm'
$Global:HAO_ANTLR = Join-Path $HAO_ROOT 'lib\antlr4'
$Global:ANTLR_JAR = Join-Path $HAO_ANTLR 'antlr-4.13.2-complete.jar'

# 可选本机代理文件
$proxyLocal = Join-Path $HAO_ROOT 'script\win\proxy.local.ps1'
if (Test-Path -LiteralPath $proxyLocal) { . $proxyLocal }

# PS5.1：避免 PS7 模块混入导致 scoop/Get-FileHash 异常
if ($PSVersionTable.PSVersion.Major -eq 5) {
    $sysMods = Join-Path $env:WINDIR 'system32\WindowsPowerShell\v1.0\Modules'
    $scoopMods = Join-Path $env:USERPROFILE 'scoop\modules'
    # 兼容旧 scoop 自定义根目录
    if (-not (Test-Path $scoopMods) -and $env:SCOOP) {
        $scoopMods = Join-Path $env:SCOOP 'modules'
    }
    $parts = @($sysMods)
    if (Test-Path $scoopMods) { $parts += $scoopMods }
    $env:PSModulePath = ($parts -join ';')
}

# java：JAVA_HOME 优先补 PATH
if ($env:JAVA_HOME) {
    $javaBin = Join-Path $env:JAVA_HOME 'bin'
    if ((Test-Path $javaBin) -and ($env:PATH -notlike "*$javaBin*")) {
        $env:PATH = "$javaBin;$env:PATH"
    }
}

# scoop shims（若已装）
$scoopShim = Join-Path $env:USERPROFILE 'scoop\shims'
if (-not (Test-Path $scoopShim) -and $env:SCOOP) {
    $scoopShim = Join-Path $env:SCOOP 'shims'
}

$paths = @(
    (Join-Path $HAO_LLVM 'bin')   # clang / lld / llvm-*
    $scoopShim                    # cmake / ninja / python / 7z / git …
    (Join-Path $HAO_ROOT 'output')
)
foreach ($p in $paths) {
    if ($p -and (Test-Path $p) -and ($env:PATH -notlike "*$p*")) {
        $env:PATH = "$p;$env:PATH"
    }
}

function Enable-HaoProxy {
    $url = $env:HAO_HTTP_PROXY
    if (-not $url) { $url = $env:https_proxy }
    if (-not $url) { $url = $env:http_proxy }
    if (-not $url) {
        Write-Host '[代理] 未配置。复制 script\win\proxy.local.ps1.example 为 script\win\proxy.local.ps1，或设 HAO_HTTP_PROXY' -ForegroundColor Yellow
        return
    }
    $env:http_proxy  = $url
    $env:https_proxy = $url
    $env:HTTP_PROXY  = $url
    $env:HTTPS_PROXY = $url
    $env:HAO_HTTP_PROXY = $url
    Write-Host "[代理] 已开启 $url" -ForegroundColor Yellow
}
function Disable-HaoProxy {
    Remove-Item env:http_proxy, env:https_proxy, env:HTTP_PROXY, env:HTTPS_PROXY, env:HAO_HTTP_PROXY -ErrorAction Ignore
    Write-Host '[代理] 已关闭' -ForegroundColor Yellow
}

function Invoke-AntlrGen {
    $astDir = Join-Path $HAO_ROOT 'src\ast'
    $outDir = Join-Path $astDir 'generated'
    $jar    = $ANTLR_JAR
    if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
        Write-Host '[ANTLR] 未找到 java。设 JAVA_HOME 或 scoop install temurin-lts-jdk' -ForegroundColor Red
        return
    }
    if (-not (Test-Path $jar)) {
        Write-Host "[ANTLR] 缺少 $jar" -ForegroundColor Red
        return
    }
    Write-Host '[ANTLR] 生成 Lexer...' -ForegroundColor Cyan
    java -jar $jar -Dlanguage=Cpp -o $outDir (Join-Path $astDir 'HaoLangLexer.g4')
    if ($LASTEXITCODE -ne 0) { Write-Host '[ANTLR] Lexer 生成失败' -ForegroundColor Red; return }
    Write-Host '[ANTLR] 生成 Parser...' -ForegroundColor Cyan
    java -jar $jar -Dlanguage=Cpp -visitor -no-listener -lib $outDir -o $outDir `
        (Join-Path $astDir 'HaoLangParser.g4')
    if ($LASTEXITCODE -ne 0) { Write-Host '[ANTLR] Parser 生成失败' -ForegroundColor Red; return }
    Write-Host "[ANTLR] 生成完成 -> $outDir" -ForegroundColor Green
}
Set-Alias antlrgen Invoke-AntlrGen -Scope Global -Force

function Invoke-HaoBuild {
    param([switch]$Fresh)
    # 正斜杠：路径含 \b（如 D:\buildLang）时 cmake 写 RC 缓存会当转义失败
    $clangxx = ((Join-Path $HAO_LLVM 'bin\clang++.exe') -replace '\\', '/')
    $llvmrc  = ((Join-Path $HAO_LLVM 'bin\llvm-rc.exe') -replace '\\', '/')
    if (-not (Test-Path ($clangxx -replace '/', '\'))) {
        Write-Host '[构建] 缺少 lib\llvm。请先: powershell -File script\setup_env.ps1' -ForegroundColor Red
        return
    }
    $build = Join-Path $HAO_ROOT 'build'
    if ($Fresh -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
    cmake -S $HAO_ROOT -B $build -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_CXX_COMPILER="$clangxx" `
        -DCMAKE_RC_COMPILER="$llvmrc"
    if ($LASTEXITCODE -ne 0) { return }
    cmake --build $build --parallel 8
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[构建] 完成 -> $HAO_ROOT\output\hao.exe" -ForegroundColor Green
    }
}
Set-Alias haobuild Invoke-HaoBuild -Scope Global -Force

function Test-HaoEnv {
    Write-Host ''
    Write-Host '===== HaoLang 环境自检 =====' -ForegroundColor Cyan
    $checks = [ordered]@{
        'clang'  = { (& clang --version 2>&1)    | Select-Object -First 1 }
        'lld'    = { (& lld-link --version 2>&1) | Select-Object -First 1 }
        'cmake'  = { (& cmake --version 2>&1)    | Select-Object -First 1 }
        'ninja'  = { "ninja $(& ninja --version 2>&1)" }
        'java'   = { (& java --version 2>&1)     | Select-Object -First 1 }
        'python' = {
            if (Get-Command python -ErrorAction SilentlyContinue) {
                (& python --version 2>&1) | Select-Object -First 1
            } else {
                (& python3 --version 2>&1) | Select-Object -First 1
            }
        }
        '7z'     = { (& 7z 2>&1 | Select-Object -First 2) -join ' ' }
        'git'    = { (& git --version 2>&1) | Select-Object -First 1 }
    }
    foreach ($name in $checks.Keys) {
        try {
            $v = & $checks[$name]
            if ([string]::IsNullOrWhiteSpace($v)) { throw '无输出' }
            Write-Host ('  {0,-8} OK   {1}' -f $name, $v) -ForegroundColor Green
        } catch {
            Write-Host ('  {0,-8} 缺失' -f $name) -ForegroundColor Red
        }
    }
    # MSVC STL（本机 VS，非 LLVM）
    $pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
    if ([string]::IsNullOrEmpty($pf86)) { $pf86 = 'C:\Program Files (x86)' }
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msvcOk = $false
    $msvcMsg = ''
    if (Test-Path -LiteralPath $vswhere) {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $vsRoot = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        $ErrorActionPreference = $prevEap
        if ($vsRoot) {
            $msvcDir = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
            if ($msvcDir -and (Test-Path (Join-Path $msvcDir.FullName 'include\vector'))) {
                $msvcOk = $true
                $msvcMsg = "$($msvcDir.Name) @ $vsRoot"
            }
        }
    }
    if ($msvcOk) {
        Write-Host ('  {0,-8} OK   {1}' -f 'msvc', $msvcMsg) -ForegroundColor Green
    } else {
        Write-Host ('  {0,-8} 缺失（script\win\fetch_msvc.ps1 或 setup_env）' -f 'msvc') -ForegroundColor Red
    }
    if (Test-Path $ANTLR_JAR) {
        Write-Host ('  {0,-8} OK   {1}' -f 'antlr', (Split-Path $ANTLR_JAR -Leaf)) -ForegroundColor Green
    } else {
        Write-Host ('  {0,-8} 缺失' -f 'antlr') -ForegroundColor Red
    }
    $lib = Join-Path $HAO_ANTLR 'install\lib\libantlr4-runtime.a'
    if (Test-Path $lib) {
        $sz = [math]::Round((Get-Item $lib).Length / 1MB, 1)
        Write-Host ('  {0,-8} OK   libantlr4-runtime.a ({1} MB)' -f 'antlr-rt', $sz) -ForegroundColor Green
    } else {
        Write-Host ('  {0,-8} 未编译' -f 'antlr-rt') -ForegroundColor Red
    }
    $crt = Join-Path $HAO_ROOT 'lib\sysroot\win-amd64\lib\libcmt.lib'
    if (Test-Path $crt) {
        Write-Host ('  {0,-8} OK   win-amd64 CRT' -f 'winlibs') -ForegroundColor Green
    } else {
        Write-Host ('  {0,-8} 缺失（script\win\fetch_winlibs.ps1）' -f 'winlibs') -ForegroundColor Red
    }
    $musl = Join-Path $HAO_ROOT 'lib\sysroot\linux-amd64\include\stdio.h'
    if (Test-Path $musl) {
        Write-Host ('  {0,-8} OK   linux-amd64 musl' -f 'sysroot') -ForegroundColor Green
    } else {
        Write-Host ('  {0,-8} 缺失（script\win\fetch_sysroot.ps1）' -f 'sysroot') -ForegroundColor Yellow
    }
    Write-Host ''
}
Set-Alias haoenv Test-HaoEnv -Scope Global -Force

Write-Host ''
Write-Host '  HaoLang 开发环境已加载' -ForegroundColor Cyan
Write-Host "  根目录: $HAO_ROOT"
Write-Host ''
Write-Host '  可用命令:' -ForegroundColor Cyan
Write-Host '    haoenv            环境自检'
Write-Host '    haobuild          构建 hao.exe（-Fresh 清理重建）'
Write-Host '    antlrgen          从 .g4 重新生成 C++ 分析器'
Write-Host '    Enable-HaoProxy   开启下载代理（读 HAO_HTTP_PROXY / proxy.local.ps1）'
Write-Host '    Disable-HaoProxy  关闭下载代理'
Write-Host ''