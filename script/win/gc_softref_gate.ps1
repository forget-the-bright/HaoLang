# GC-ALIGN-5 A: SoftRef 不强清强活（独立节点；禁全量）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing hao.exe"; exit 1 }

powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { exit 1 }

$env:HAO_GC_VERIFY = "1"
$out = & $hao run (Join-Path $Root "test\gc_softref_strong_smoke.hao") 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue
Write-Host $out
if ($code -ne 0) { Write-Host "FAIL gc_softref_strong_smoke exit=$code"; exit 1 }
$trues = @(($out -split "`r?`n") | Where-Object { $_.Trim() -eq "true" })
if ($trues.Count -lt 2) { Write-Host "FAIL expect 2 true got $($trues.Count)"; exit 1 }
Write-Host "OK   gc_softref_strong_smoke ($($trues.Count) true)"
Write-Host "GC_SOFTREF_GATE OK"
exit 0
