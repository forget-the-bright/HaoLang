# T03：IRGen TRACE 统一 helper 门禁（独立；禁全量套件）
# 用法（仓库根）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\trace_helper_gate.ps1
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$fail = 0
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { throw "missing $hao" }

# 1) 业务文件禁散落 fprintf(...hao:irgen
$bad = Select-String -Path (Join-Path $Root "src\irgen\*.cpp") -Pattern 'fprintf\s*\(\s*stderr'
# 允许仅 IRGen.cpp 的 vfprintf 实现
$bad = $bad | Where-Object { $_.Path -notmatch 'IRGen\.cpp$' -or $_.Line -notmatch 'vfprintf' }
$bad = $bad | Where-Object { $_.Line -match 'hao:irgen' -or $_.Line -match 'fprintf\s*\(\s*stderr' }
# IRGen.cpp 的 vfprintf 合法；其它 cpp 的 fprintf(stderr) 全禁
$bad2 = Select-String -Path (Join-Path $Root "src\irgen\IRGenValue.cpp"),(Join-Path $Root "src\irgen\IRGenExcept.cpp"),(Join-Path $Root "src\irgen\IRGenControl.cpp"),(Join-Path $Root "src\irgen\IRGenExpr.cpp"),(Join-Path $Root "src\irgen\IRGenClass.cpp"),(Join-Path $Root "src\irgen\IRGenLambda.cpp"),(Join-Path $Root "src\irgen\IRGenLiteral.cpp") -Pattern 'fprintf\s*\(\s*stderr' -ErrorAction SilentlyContinue
if ($bad2) {
    Write-Host "FAIL TRACE_GATE business fprintf(stderr):"
    $bad2 | ForEach-Object { Write-Host "  $($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }
    $fail++
} else {
    Write-Host "OK   no business fprintf(stderr) in IRGen*.cpp"
}

# 2) 必须有统一入口
$has = Select-String -Path (Join-Path $Root "src\irgen\IRGen.cpp") -Pattern 'void IRGen::traceIrgen\('
if (-not $has) {
    Write-Host "FAIL TRACE_GATE missing IRGen::traceIrgen"
    $fail++
} else {
    Write-Host "OK   IRGen::traceIrgen present"
}

# 3) TRACE=1 时小例仍见前缀
$td = Join-Path $Root "target\test\trace_helper_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$src = Join-Path $td "t.hao"
@'
package main;
import collections.*;
func main() {
    var a = new ArrayList<String>();
    var i = 0;
    while (i < 3) {
        a.add("x");
        i = i + 1;
    }
}
'@ | Set-Content -Encoding utf8 $src
$env:HAO_IRGEN_TRACE = "1"
$ErrorActionPreference = "Continue"
$out = (& $hao emit $src -o (Join-Path $td "t.ll") 2>&1 | ForEach-Object { "$_" }) -join "`n"
$ErrorActionPreference = "Stop"
Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue
if ($out -notmatch 'hao:irgen:') {
    Write-Host "FAIL TRACE_GATE HAO_IRGEN_TRACE=1 produced no hao:irgen: prefix"
    Write-Host $out
    $fail++
} else {
    Write-Host "OK   HAO_IRGEN_TRACE emits hao:irgen: prefix"
}

if ($fail -gt 0) {
    Write-Host "TRACE_HELPER_GATE FAIL ($fail)"
    exit 1
}
Write-Host "TRACE_HELPER_GATE OK"
exit 0
