# GC-STW-1 B：STW 所有权 / 不变量（独立；禁全量）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing hao.exe"; exit 1 }

powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { exit 1 }

$ErrorActionPreference = "Continue"
$env:HAO_GC_TRACE = "1"
$out = & $hao run (Join-Path $Root "test\gc_stw_watchdog_ownership_smoke.hao") 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:HAO_GC_TRACE -ErrorAction SilentlyContinue
$ErrorActionPreference = "Stop"
Write-Host $out
if ($code -ne 0) { Write-Host "FAIL ownership smoke exit=$code"; exit 1 }
$trues = @(($out -split "`r?`n") | Where-Object { $_.Trim() -eq "true" })
if ($trues.Count -lt 5) { Write-Host "FAIL expect >=5 true got $($trues.Count)"; exit 1 }
Write-Host "OK   gc_stw_watchdog_ownership_smoke ($($trues.Count) true)"
Write-Host "GC_STW_OWNERSHIP_GATE OK"
exit 0
