# DOC-STW 软 STW 握手门禁（独立，不跑 suite）
# 用法（仓库根）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\stw_handshake_gate.ps1
# 绿条件：collect 进展 + abort/incomplete 分相不变量 + 握手税签名
# 禁止：把 stwIncomplete==0 当绿
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { throw "missing $hao" }

$src = Join-Path $Root "test\gc_stw_handshake_gate.hao"
$outdir = Join-Path $Root "target\test\stw_handshake_gate"
$exe = Join-Path $outdir "gc_stw_handshake_gate.exe"
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

Write-Host "== STW handshake gate: build =="
& $hao build $src -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL STW_HANDSHAKE build"
    exit $LASTEXITCODE
}

Write-Host "== STW handshake gate: run =="
$out = & $exe 2>&1 | Out-String
Write-Host $out
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL STW_HANDSHAKE exit=$LASTEXITCODE"
    exit $LASTEXITCODE
}

$lines = @($out -split "`r?`n" | Where-Object { $_.Trim() -ne "" })
# 前 5 行须为 true（dCollect/invAbort/invInc/taxOk/markAll）
$trueCount = 0
foreach ($ln in $lines) {
    if ($ln.Trim() -eq "true") { $trueCount++ }
    if ($ln -match "^GATE ") { break }
}
if ($trueCount -lt 5) {
    Write-Host "FAIL STW_HANDSHAKE expected >=5 true lines, got $trueCount"
    exit 1
}

$gate = ($lines | Where-Object { $_ -match "^GATE " } | Select-Object -First 1)
if (-not $gate) {
    Write-Host "FAIL STW_HANDSHAKE missing GATE diagnostic line"
    exit 1
}
if ($gate -notmatch "collectDelta=([1-9][0-9]*)") {
    Write-Host "FAIL STW_HANDSHAKE collectDelta must be >=1 (got: $gate)"
    exit 1
}
# 显式拒绝「incomplete==0 才绿」：脚本只校验有 GATE 行与 collect 进展
Write-Host "OK   STW_HANDSHAKE (collect progressed; phase invariants; incomplete==0 NOT required)"
Write-Host "STW_HANDSHAKE_GATE OK"
exit 0
