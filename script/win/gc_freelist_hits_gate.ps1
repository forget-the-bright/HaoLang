# B2: freelistHits / spanSweepChunks rise after reuse (no full suite)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing output\hao.exe"; exit 1 }

powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL build_runtime"; exit 1 }

$out = & $hao run (Join-Path $Root "test\gc_freelist_hits_smoke.hao") 2>&1 | Out-String
$code = $LASTEXITCODE
Write-Host $out
if ($code -ne 0) {
    Write-Host "FAIL gc_freelist_hits_smoke exit=$code"
    exit 1
}
$lines = ($out -split "`r?`n") | Where-Object { $_.Trim() -ne "" }
$trueLines = @($lines | Where-Object { $_.Trim() -eq "true" })
if ($trueLines.Count -lt 3) {
    Write-Host "FAIL expected 3x true, got $($trueLines.Count)"
    exit 1
}
Write-Host "OK   gc_freelist_hits_smoke"
Write-Host "GC_FREELIST_HITS_GATE OK"
exit 0
