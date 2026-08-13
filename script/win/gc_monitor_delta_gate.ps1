# GC-STW-1：轮询 monitor /api/gc，仅在 collectCount 上升时打印 per-GC 增量
# 用法（仓库根，monitor 已监听 :18090）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\gc_monitor_delta_gate.ps1
# 可选：-Url -Seconds -IntervalMs
param(
    [string]$Url = "http://127.0.0.1:18090/api/gc",
    [int]$Seconds = 90,
    [int]$IntervalMs = 500
)
$ErrorActionPreference = "Stop"

function Get-Snap {
    $resp = Invoke-WebRequest -Uri $Url -Method Get -TimeoutSec 3
    return ($resp.Content | ConvertFrom-Json)
}

Write-Host "GC_MONITOR_DELTA poll $Url for ${Seconds}s (interval ${IntervalMs}ms)"
Write-Host "Only rows with DeltaCollect>=1 are printed; incomplete==0 NOT required"

$prev = $null
$end = (Get-Date).AddSeconds($Seconds)
$deltaRows = 0
$sumAbort = 0
$sumCollect = 0
$sumHeap = 0

while ((Get-Date) -lt $end) {
    try {
        $cur = Get-Snap
    } catch {
        Write-Host "WARN poll fail: $($_.Exception.Message)"
        Start-Sleep -Milliseconds $IntervalMs
        continue
    }
    if ($null -eq $prev) {
        $prev = $cur
        Start-Sleep -Milliseconds $IntervalMs
        continue
    }
    $dCollect = [int64]$cur.collectCount - [int64]$prev.collectCount
    if ($dCollect -ge 1) {
        $dInc = [int64]$cur.stwIncomplete - [int64]$prev.stwIncomplete
        $dIncR = [int64]$cur.stwIncompleteRoot - [int64]$prev.stwIncompleteRoot
        $dIncT = [int64]$cur.stwIncompleteTerm - [int64]$prev.stwIncompleteTerm
        $dAbort = [int64]$cur.markAbortCycles - [int64]$prev.markAbortCycles
        $dAbR = [int64]$cur.markAbortRoot - [int64]$prev.markAbortRoot
        $dAbT = [int64]$cur.markAbortTerm - [int64]$prev.markAbortTerm
        $dAbP = [int64]$cur.markAbortParkWd - [int64]$prev.markAbortParkWd
        $dPark = [int64]$cur.parkWatchdogTrips - [int64]$prev.parkWatchdogTrips
        $dHeap = [int64]$cur.heapBytes - [int64]$prev.heapBytes
        $dLive = [int64]$cur.liveBytes - [int64]$prev.liveBytes
        $dBlocks = [int64]$cur.blockCount - [int64]$prev.blockCount
        $dWait = 0
        if ($null -ne $cur.stwWaitMsTotal -and $null -ne $prev.stwWaitMsTotal) {
            $dWait = [int64]$cur.stwWaitMsTotal - [int64]$prev.stwWaitMsTotal
        }
        $dFl = 0
        if ($null -ne $cur.freelistHits -and $null -ne $prev.freelistHits) {
            $dFl = [int64]$cur.freelistHits - [int64]$prev.freelistHits
        }
        $dCommit = 0
        if ($null -ne $cur.spanCommitBytes -and $null -ne $prev.spanCommitBytes) {
            $dCommit = [int64]$cur.spanCommitBytes - [int64]$prev.spanCommitBytes
        }
        $hold = 0
        if ($null -ne $cur.lastCollectHoldMs) { $hold = [int64]$cur.lastCollectHoldMs }
        $statsWait = 0
        if ($null -ne $cur.lastStatsLockWaitMs) { $statsWait = [int64]$cur.lastStatsLockWaitMs }
        $abortRate = if ($dCollect -gt 0) { [math]::Round(($dAbort * 1.0) / $dCollect, 3) } else { 0 }
        Write-Host ("DELTA collect=+$dCollect abort=+$dAbort (r=$dAbR t=$dAbT pwd=$dAbP) " +
            "inc=+$dInc (r=$dIncR t=$dIncT) parkTrip=+$dPark " +
            "heap=$dHeap live=$dLive blocks=$dBlocks waitMs=+$dWait fl=+$dFl commitΔ=$dCommit " +
            "hold=$hold statsWait=$statsWait unlink=$($cur.lastUnlinkMs) drain=$($cur.lastDrainMs) " +
            "abortRate=$abortRate thr=$($cur.threshold) " +
            "lastStw=p$($cur.lastStwPhase)/miss$($cur.lastStwMissing)/$($cur.lastStwTargets)")
        $deltaRows++
        $sumAbort += $dAbort
        $sumCollect += $dCollect
        $sumHeap += $dHeap
        $prev = $cur
    }
    Start-Sleep -Milliseconds $IntervalMs
}

Write-Host "=================================================="
Write-Host "SUMMARY deltaRows=$deltaRows sumCollect=+$sumCollect sumAbort=+$sumAbort sumHeapDelta=$sumHeap"
if ($sumCollect -gt 0) {
    Write-Host ("SUMMARY abortRate=" + [math]::Round(($sumAbort * 1.0) / $sumCollect, 3))
}
if ($deltaRows -lt 1) {
    Write-Host "FAIL GC_MONITOR_DELTA no collect deltas (is monitor running with load?)"
    exit 1
}
Write-Host "GC_MONITOR_DELTA_GATE OK"
exit 0
