# B1: HAO_GC_VERIFY=1 + span freelist smoke (no full suite)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing output\hao.exe"; exit 1 }

# rebuild runtime so VERIFY freelist is linked
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL build_runtime"; exit 1 }

$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $Root "test\gc_span_sweep_smoke.hao")
$code = $LASTEXITCODE
Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue
if ($code -ne 0) {
    Write-Host "FAIL gc_span_sweep_smoke under HAO_GC_VERIFY=1 exit=$code"
    exit 1
}
Write-Host "OK   HAO_GC_VERIFY=1 gc_span_sweep_smoke"
Write-Host "GC_VERIFY_FREELIST_GATE OK"
exit 0
