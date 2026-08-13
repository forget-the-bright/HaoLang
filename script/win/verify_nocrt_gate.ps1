# E01：VERIFY 路径禁 snprintf（runtime_gc.c）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$fail = 0
$gc = Join-Path $Root "stdlib\runtime_gc.c"
$lines = Get-Content $gc
$ln = 0
foreach ($line in $lines) {
    $ln++
    if ($line -match 'snprintf\s*\(') {
        Write-Host "FAIL runtime_gc.c:${ln}: $($line.Trim())"
        $fail++
    }
}
if ($fail -eq 0) { Write-Host "OK   runtime_gc.c has zero snprintf(" }

$hao = Join-Path $Root "output\hao.exe"
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $Root "test\gc_concurrent_sweep_smoke.hao") 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL VERIFY collect smoke exit=$LASTEXITCODE"
    $fail++
} else { Write-Host "OK   HAO_GC_VERIFY=1 concurrent sweep smoke" }
Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue

if ($fail -gt 0) { Write-Host "VERIFY_NOCRT_GATE FAIL ($fail)"; exit 1 }
Write-Host "VERIFY_NOCRT_GATE OK"
exit 0
