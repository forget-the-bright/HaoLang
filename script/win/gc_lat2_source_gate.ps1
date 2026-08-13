# GC-LAT-2：源码门禁 — 必须有短批让路 + 发布式 memstats；禁单锁贯穿回潮
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$gc = Get-Content -Raw (Join-Path $Root "stdlib\runtime_gc.c")
$fail = 0

function Need([string]$pat, [string]$msg) {
    if ($script:gc -notmatch $pat) {
        Write-Host "FAIL GC_LAT2 $msg"
        $script:fail++
    } else {
        Write-Host "OK   $msg"
    }
}

Need "gc_stats_publish_locked" "missing stats publish"
Need "g_stats_snap" "missing memstats snapshot buffer"
Need "unlink_batch" "missing unlink_batch TRACE/yield"
Need "drain_batch" "missing drain_batch yield"
Need "GC_SWEEP_SLICE_MS" "missing sweep time slice"
Need "lastCollectHoldMs|g_last_collect_hold_ms" "missing collect hold probe"
# 禁止任一 collect 入口 trampoline 后立即 drain（须 publish/unlock 窗口）
$bad = [regex]::Matches($gc, "gc_collect_trampoline\(\);\s*\r?\n\s*gc_span_drain_doomed_locked")
if ($bad.Count -gt 0) {
    Write-Host "FAIL GC_LAT2 collect still chains trampoline→drain without unlock window ($($bad.Count))"
    $fail++
} else {
    Write-Host "OK   collect unlock window before drain"
}
if ($gc -match "hao_win_suspend|SuspendThread\s*\(") {
    Write-Host "FAIL GC_LAT2 SuspendThread must stay deleted"
    $fail++
} else {
    Write-Host "OK   no SuspendThread"
}

if ($fail -gt 0) {
    Write-Host "GC_LAT2_SOURCE_GATE FAIL ($fail)"
    exit 1
}
Write-Host "GC_LAT2_SOURCE_GATE OK"
exit 0
