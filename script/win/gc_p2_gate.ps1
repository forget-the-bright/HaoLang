# GC-ALIGN-4: weakref + rates/boot smoke (+ VERIFY freelist already gated)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { Write-Host "FAIL missing hao.exe"; exit 1 }

powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_runtime.ps1")
if ($LASTEXITCODE -ne 0) { exit 1 }

function Run-Smoke([string]$rel) {
    $env:HAO_GC_VERIFY = "1"
    $out = & $hao run (Join-Path $Root $rel) 2>&1 | Out-String
    $code = $LASTEXITCODE
    Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue
    Write-Host $out
    if ($code -ne 0) { Write-Host "FAIL $rel exit=$code"; exit 1 }
    $trues = @(($out -split "`r?`n") | Where-Object { $_.Trim() -eq "true" })
    if ($trues.Count -lt 1) { Write-Host "FAIL $rel no true"; exit 1 }
    Write-Host "OK   $rel ($($trues.Count) true)"
}

Run-Smoke "test\gc_weakref_smoke.hao"
Run-Smoke "test\gc_softref_strong_smoke.hao"
Run-Smoke "test\gc_oom_throw_smoke.hao"
Run-Smoke "test\gc_bitmap_wide_smoke.hao"
Write-Host "GC_P2_GATE OK"
exit 0
