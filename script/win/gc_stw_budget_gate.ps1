# GC-LAT-1：源码预算上限门禁 — 禁止再加长软 STW / 恢复 HOT_GRACE / SuspendThread
# 用法（仓库根）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\gc_stw_budget_gate.ps1
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$gc = Join-Path $Root "stdlib\runtime_gc.c"
$win = Join-Path $Root "stdlib\runtime_winapi.c"
if (-not (Test-Path $gc)) { throw "missing $gc" }

$src = Get-Content -Raw -Path $gc
$winSrc = ""
if (Test-Path $win) { $winSrc = Get-Content -Raw -Path $win }

function Assert-Define([string]$name, [int]$max) {
    if ($src -notmatch "(?m)^#define\s+$([regex]::Escape($name))\s+(\d+)") {
        Write-Host "FAIL GC_STW_BUDGET missing #define $name"
        exit 1
    }
    $val = [int]$Matches[1]
    if ($val -gt $max) {
        Write-Host "FAIL GC_STW_BUDGET $name=$val > max $max (do not lengthen soft STW)"
        exit 1
    }
    Write-Host "OK   $name=$val (max $max)"
}

Assert-Define "GC_STW_TOTAL_MS" 16
Assert-Define "GC_STW_TERM_TOTAL_MS" 12
Assert-Define "GC_STW_ROUND_MS" 6
Assert-Define "GC_STW_ROUNDS" 2
Assert-Define "GC_STW_TERM_RETRIES" 2
Assert-Define "GC_STW_STALL_MS" 4

if ($src -match "(?m)^#define\s+GC_STW_HOT_GRACE" -or $src -match "(?m)^#define\s+GC_STW_HOT_MISS") {
    Write-Host "FAIL GC_STW_BUDGET hot-grace macros must stay deleted"
    exit 1
}
if ($src -match '"stw_grace_rescue') {
    Write-Host "FAIL GC_STW_BUDGET stw_grace_rescue TRACE path must stay deleted"
    exit 1
}
if ($src -match "g_stw_grace_rescues\s*\+\+") {
    Write-Host "FAIL GC_STW_BUDGET grace rescue counter must not increment"
    exit 1
}
if ($src -match "SuspendThread" -or $winSrc -match "SuspendThread") {
    Write-Host "FAIL GC_STW_BUDGET SuspendThread must not return"
    exit 1
}
if ($src -match "hao_win_suspend" -or $winSrc -match "hao_win_suspend") {
    Write-Host "FAIL GC_STW_BUDGET hao_win_suspend must not return"
    exit 1
}

Write-Host "GC_STW_BUDGET_GATE OK"
exit 0
