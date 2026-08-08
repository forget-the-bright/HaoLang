# ============================================================
#  编译 HaoLang 运行时库
# ------------------------------------------------------------
#  用法:
#    script\win\build_runtime.ps1                       宿主平台 -> libhaort.a
#    script\win\build_runtime.ps1 -Target linux-amd64   交叉编译 -> libhaort-linux-amd64.a
#
#  运行时按功能拆分为 stdlib/runtime_*.c（GC/字符串/数组/对象/装箱/
#  异常/打印/panic），共享 runtime_internal.h。本脚本会自动编译所有
#  runtime_*.c 并打包进同一个静态库。修改任意一个文件后重新执行本脚本。
# ============================================================

param(
    [string]$Target = ""
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
$llvm   = "$root\lib\llvm\bin"
$stdlib = "$root\stdlib"

# ---------- 平台 -> LLVM target triple ----------
$triples = @{
    'win-amd64'    = 'x86_64-pc-windows-msvc'
    'win-arm64'    = 'aarch64-pc-windows-msvc'
    'linux-amd64'  = 'x86_64-linux-musl'
    'linux-arm64'  = 'aarch64-linux-musl'
    'darwin-amd64' = 'x86_64-apple-darwin'
    'darwin-arm64' = 'aarch64-apple-darwin'
}

$isCross = [bool]$Target
if ($isCross -and -not $triples.ContainsKey($Target)) {
    Write-Host "未知目标平台: $Target" -ForegroundColor Red
    Write-Host "支持: $($triples.Keys -join ', ')" -ForegroundColor Yellow
    exit 1
}

# 输出库名：宿主用 libhaort.a，交叉编译带平台后缀便于共存
$libName = if ($isCross) { "libhaort-$Target.a" } else { "libhaort.a" }

Write-Host "编译 HaoLang 运行时库 -> $libName" -ForegroundColor Cyan

Push-Location $stdlib
try {
    # 收集所有运行时源文件（按文件名排序，输出稳定）
    $srcFiles = Get-ChildItem -Path '.' -Filter 'runtime_*.c' | Sort-Object Name
    if ($srcFiles.Count -eq 0) { throw "未找到任何 runtime_*.c 源文件" }

    # 公共编译选项
    $commonArgs = @('-c', '-O2')
    if ($isCross) {
        $triple = $triples[$Target]
        $commonArgs += "--target=$triple"

        if (-not $Target.StartsWith('win-')) {
            # 非 Windows 目标需要 sysroot 提供 libc 头文件（stdio.h 等）
            $sysroot = "$root\lib\sysroot\$Target"
            if (-not (Test-Path "$sysroot\include\stdio.h") -and -not (Test-Path "$sysroot\usr\include\stdio.h")) {
                Write-Host "缺少 sysroot: $sysroot" -ForegroundColor Red
                Write-Host "交叉编译到 $Target 需要目标系统的 glibc 头文件与库。" -ForegroundColor Yellow
                Write-Host "这些文件不属于 LLVM，请先执行:" -ForegroundColor Yellow
                Write-Host "  script\win\fetch_sysroot.ps1 -Target $Target" -ForegroundColor Yellow
                exit 1
            }
            $commonArgs += "--sysroot=$sysroot"
        }
    } else {
        # 宿主 Windows：静态 CRT，必须与 hao.exe 及用户程序一致，
        # 否则 lld-link 报 /failifmismatch: RuntimeLibrary 冲突
        $commonArgs += @('-Xclang', '--dependent-lib=libcmt')
    }

    # 逐个编译为 .obj
    $objFiles = @()
    foreach ($src in $srcFiles) {
        $objName = if ($isCross) {
            [System.IO.Path]::ChangeExtension($src.Name, ".$Target.obj")
        } else {
            [System.IO.Path]::ChangeExtension($src.Name, '.obj')
        }
        $cargs = $commonArgs + @($src.Name, '-o', $objName)
        Write-Host "  clang $($cargs -join ' ')" -ForegroundColor DarkGray
        & "$llvm\clang.exe" @cargs
        if ($LASTEXITCODE -ne 0) { throw "编译 $($src.Name) 失败" }
        $objFiles += $objName
    }

    # 用 llvm-ar（GNU 语法）打包所有 .obj；llvm-lib 不接受 crs 参数
    if (Test-Path $libName) { Remove-Item $libName -Force }
    & "$llvm\llvm-ar.exe" crs $libName @objFiles
    if ($LASTEXITCODE -ne 0) { throw "打包 $libName 失败" }

    Remove-Item $objFiles -ErrorAction Ignore

    $size = (Get-Item $libName).Length / 1KB
    Write-Host ("完成: {0}\{1}  ({2:N1} KB, {3} 个模块)" -f $stdlib, $libName, $size, $srcFiles.Count) -ForegroundColor Green

    Write-Host "`n导出的运行时函数:" -ForegroundColor Cyan
    & "$llvm\llvm-nm.exe" --defined-only $libName 2>$null |
        Select-String ' T hao_' |
        ForEach-Object { "  " + ($_ -split '\s+')[-1] }
} finally {
    Pop-Location
}
