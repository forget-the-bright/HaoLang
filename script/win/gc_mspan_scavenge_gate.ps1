# GC-MSPAN：造压后 commit/scavenge 可观测；禁只涨假计数
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
$src = Join-Path $Root "test\gc_mspan_scavenge_smoke.hao"
$outdir = Join-Path $Root "target\test\mspan_scavenge"
$exe = Join-Path $outdir "gc_mspan_scavenge_smoke.exe"
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

# 源码：小对象不得再进 CRT 永久 freelist 头
$gc = Get-Content -Raw (Join-Path $Root "stdlib\runtime_gc.c")
if ($gc -match "g_span_free\s*=") {
    Write-Host "FAIL GC_MSPAN CRT g_span_free freelist must stay deleted"
    exit 1
}
if ($gc -notmatch "GC_MSPAN_BYTES") {
    Write-Host "FAIL GC_MSPAN missing GC_MSPAN_BYTES"
    exit 1
}
if ($gc -notmatch "allocBits") {
    Write-Host "FAIL GC_MSPAN missing allocBits"
    exit 1
}
if ($gc -notmatch "hao_os_valloc") {
    Write-Host "FAIL GC_MSPAN missing hao_os_valloc path"
    exit 1
}

Write-Host "== mspan scavenge gate: build =="
& $hao build $src -o $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== mspan scavenge gate: run =="
$out = & $exe 2>&1 | Out-String
Write-Host $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$trueCount = 0
foreach ($ln in ($out -split "`r?`n")) {
    if ($ln.Trim() -eq "true") { $trueCount++ }
    if ($ln -match "^GATE ") { break }
}
if ($trueCount -lt 4) {
    Write-Host "FAIL GC_MSPAN expected >=4 true, got $trueCount"
    exit 1
}
if ($out -notmatch "GATE ") {
    Write-Host "FAIL GC_MSPAN missing GATE line"
    exit 1
}
Write-Host "GC_MSPAN_SCAVENGE_GATE OK"
exit 0
