# GC-ALIGN-3: >32-slot precise BITMAP (no FULL) — node gate, not full suite
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing output\hao.exe"; exit 1 }

powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL build_runtime"; exit 1 }

$env:HAO_GC_VERIFY = "1"
$out = & $hao run (Join-Path $Root "test\gc_bitmap_wide_smoke.hao") 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue
Write-Host $out
if ($code -ne 0) {
    Write-Host "FAIL gc_bitmap_wide_smoke exit=$code"
    exit 1
}
if ($out -notmatch "(?m)^true\s*$") {
    Write-Host "FAIL expected true"
    exit 1
}
Write-Host "OK   gc_bitmap_wide_smoke under HAO_GC_VERIFY=1"
Write-Host "GC_BITMAP_WIDE_GATE OK"
exit 0
