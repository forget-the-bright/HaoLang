# ============================================================
#  获取交叉编译 sysroot
# ------------------------------------------------------------
#  用法:
#    script\win\fetch_sysroot.ps1 -Target linux-amd64
#
#  为什么需要 sysroot：
#    LLVM 只是编译器，不含任何平台的 C 标准库。交叉编译到 Linux
#    时，clang 能生成 .o，但链接阶段需要目标系统的：
#      - 头文件（stdio.h 等）
#      - libc（libc.a）
#      - CRT 启动文件（crt1.o / crti.o / crtbegin*.o ...）
#      - 编译器运行时（libgcc.a）
#    这些都不在 LLVM 发行包里，必须单独准备。
#
#  为什么选 musl 而非 glibc：
#    musl 专为静态链接设计，产出的可执行文件真正零依赖，
#    符合 HaoLang 的绿色分发目标。glibc 静态链接存在
#    NSS / dlopen 等已知问题。
# ============================================================

param(
    [Parameter(Mandatory = $true)]
    [string]$Target,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
. (Join-Path $PSScriptRoot 'hao_net.ps1')
Import-HaoProxyLocal -Root $root
$srBase  = "$root\lib\sysroot"
$srDir   = "$srBase\$Target"

# ---------- 各平台的 musl 交叉工具链下载地址 ----------
$sources = @{
    'linux-amd64' = @{
        Url     = 'https://musl.cc/x86_64-linux-musl-cross.tgz'
        Inner   = 'x86_64-linux-musl-cross'
        SysDir  = 'x86_64-linux-musl'
        GccVer  = '11.2.1'
        GccArch = 'x86_64-linux-musl'
    }
    'linux-arm64' = @{
        Url     = 'https://musl.cc/aarch64-linux-musl-cross.tgz'
        Inner   = 'aarch64-linux-musl-cross'
        SysDir  = 'aarch64-linux-musl'
        GccVer  = '11.2.1'
        GccArch = 'aarch64-linux-musl'
    }
}

if (-not $sources.ContainsKey($Target)) {
    Write-Host "不支持的目标平台: $Target" -ForegroundColor Red
    Write-Host "当前支持: $($sources.Keys -join ', ')" -ForegroundColor Yellow
    Write-Host "Windows CRT 最小集请用: script\win\fetch_winlibs.ps1 -Target win-amd64|win-arm64" -ForegroundColor DarkGray
    Write-Host "macOS 需 Apple SDK，受许可限制无法自动下载。" -ForegroundColor DarkGray
    exit 1
}

$cfg = $sources[$Target]

# ---------- 已存在则跳过 ----------
if ((Test-Path "$srDir\include\stdio.h") -and -not $Force) {
    Write-Host "sysroot 已存在: $srDir" -ForegroundColor Green
    Write-Host "（使用 -Force 可强制重新下载）" -ForegroundColor DarkGray
    exit 0
}

New-Item -ItemType Directory -Path $srBase -Force | Out-Null
$tgz = "$srBase\$Target-toolchain.tgz"

# ---------- 下载 ----------
Write-Host "下载 $Target 交叉工具链（约 110 MB）..." -ForegroundColor Cyan
Write-Host "  $($cfg.Url)" -ForegroundColor DarkGray

if (-not (Test-HaoNetwork -Url 'https://musl.cc')) {
    if (-not (Test-HaoNetwork -Url 'https://github.com')) {
        Write-Host '[网络] 下载失败风险高。请配置 script\win\proxy.local.ps1 或 HAO_HTTP_PROXY 后重试' -ForegroundColor Yellow
    }
}

Invoke-HaoDownload -Url $cfg.Url -OutFile $tgz
if (-not (Test-Path $tgz)) { throw "下载文件不存在: $tgz" }

# ---------- 解压 ----------
Write-Host "解压..." -ForegroundColor Cyan
Push-Location $srBase
try {
    # .tgz 需两步：先 gzip 解出 tar，再展开 tar
    & 7z x -y $tgz -so | & 7z x -y -si -ttar -o"$srBase" | Out-Null
    if (-not (Test-Path "$srBase\$($cfg.Inner)")) { throw "解压后未找到 $($cfg.Inner)" }

    # ---------- 提取 sysroot ----------
    Write-Host "整理 sysroot..." -ForegroundColor Cyan
    if (Test-Path $srDir) { Remove-Item $srDir -Recurse -Force }
    New-Item -ItemType Directory -Path $srDir -Force | Out-Null

    $inner = "$srBase\$($cfg.Inner)"
    Copy-Item "$inner\$($cfg.SysDir)\include" $srDir -Recurse
    Copy-Item "$inner\$($cfg.SysDir)\lib"     $srDir -Recurse

    # GCC 运行时与 CRT 启动文件不在 sysroot 里，需一并拷入 lib，
    # 否则 lld 会报 cannot open crtbeginT.o / unable to find -lgcc
    $gccDir = "$inner\lib\gcc\$($cfg.GccArch)\$($cfg.GccVer)"
    if (-not (Test-Path $gccDir)) { throw "未找到 GCC 运行时目录: $gccDir" }
    Copy-Item "$gccDir\crt*.o"      "$srDir\lib\" -Force
    Copy-Item "$gccDir\libgcc*.a"   "$srDir\lib\" -Force

    # ---------- 清理 ----------
    Remove-Item $inner -Recurse -Force
    Remove-Item $tgz -Force
} finally {
    Pop-Location
}

# ---------- 自检 ----------
Write-Host "`n验证 sysroot..." -ForegroundColor Cyan
$checks = @('include\stdio.h', 'lib\libc.a', 'lib\crt1.o', 'lib\crtbeginT.o', 'lib\libgcc.a')
$ok = $true
foreach ($c in $checks) {
    if (Test-Path "$srDir\$c") {
        Write-Host "  OK   $c" -ForegroundColor Green
    } else {
        Write-Host "  缺失 $c" -ForegroundColor Red
        $ok = $false
    }
}
if (-not $ok) { throw "sysroot 不完整" }

# 用一个最小 C 程序验证能否真正链接出 ELF
$probe = "$srBase\_probe.c"
@'
#include <stdio.h>
int main(void) { printf("ok\n"); return 0; }
'@ | Set-Content $probe -Encoding ASCII

$triple = if ($Target -eq 'linux-amd64') { 'x86_64-linux-musl' } else { 'aarch64-linux-musl' }
& "$root\lib\llvm\bin\clang.exe" --target=$triple --sysroot=$srDir `
    -fuse-ld=lld -static $probe -o "$srBase\_probe.out" 2>&1 | Out-Null

if (Test-Path "$srBase\_probe.out") {
    $magic = [System.IO.File]::ReadAllBytes("$srBase\_probe.out")[0..3]
    $isElf = ($magic[0] -eq 0x7F -and $magic[1] -eq 0x45 -and
              $magic[2] -eq 0x4C -and $magic[3] -eq 0x46)
    Remove-Item "$srBase\_probe.out", $probe -Force
    if ($isElf) {
        Write-Host "  OK   链接测试通过（产出有效 ELF）" -ForegroundColor Green
    } else {
        throw "链接产物不是 ELF 格式"
    }
} else {
    Remove-Item $probe -Force -ErrorAction Ignore
    throw "链接测试失败"
}

$sizeMB = (Get-ChildItem $srDir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("`nsysroot 就绪: {0}  ({1:N0} MB)" -f $srDir, $sizeMB) -ForegroundColor Green
Write-Host "`n现在可以交叉编译了:" -ForegroundColor Cyan
Write-Host "  script\win\build_runtime.ps1 -Target $Target"
Write-Host "  hao build test\hello.hao --target $Target"
