# GC-HTTP-LAT：全量 /api/gc 延迟门禁（HttpClient + 头计时；禁靠砍字段装绿）
# 真验收：nurseryBytes 等全量字段 + hold / safepoint / serverTotal / jsonMs（头优先）
param(
    [string]$Url = "http://127.0.0.1:18090/api/gc",
    [int]$Samples = 40,
    [int]$MaxHoldP95Ms = 20,
    [int]$MaxSafepointP95Ms = 20,
    [int]$MaxServerP95Ms = 40,
    [int]$MaxJsonP95Ms = 20,
    [int]$MaxWallP95Ms = 80,
    [switch]$StartMonitor
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

Add-Type -AssemblyName System.Net.Http

$monProc = $null
if ($StartMonitor) {
    Get-Process | Where-Object { $_.ProcessName -match "08-gc-monitor" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    $hao = Join-Path $Root "output\hao.exe"
    $src = Join-Path $Root "haolang-example\08-gc-monitor"
    $exe = Join-Path $Root "target\test\gc_api_latency\08-gc-monitor.exe"
    New-Item -ItemType Directory -Force -Path (Split-Path $exe) | Out-Null
    & $hao build $src -o $exe
    if ($LASTEXITCODE -ne 0) { throw "monitor build fail" }
    $env:HAO_GC_MONITOR_WARM_BATCH = "0"
    $env:HAO_GC_MONITOR_WARM_PAUSE_MS = "60000"
    # 默认 keep-alive；勿设 HAO_HTTP_API_SHORT
    Remove-Item Env:HAO_HTTP_API_SHORT -ErrorAction SilentlyContinue
    $monProc = Start-Process -FilePath $exe -WorkingDirectory $Root -PassThru -WindowStyle Hidden
    $ready = $false
    $probe = [System.Net.Http.HttpClient]::new()
    $probe.Timeout = [TimeSpan]::FromSeconds(1)
    for ($w = 0; $w -lt 60; $w++) {
        Start-Sleep -Milliseconds 250
        try {
            $r = $probe.GetAsync($Url).GetAwaiter().GetResult()
            if ($r.IsSuccessStatusCode) { $ready = $true; break }
        } catch { }
        if ($monProc.HasExited) {
            Write-Host "FAIL GC_API_LATENCY monitor exited early code=$($monProc.ExitCode)"
            exit 1
        }
    }
    $probe.Dispose()
    if (-not $ready) {
        Write-Host "FAIL GC_API_LATENCY monitor not ready at $Url"
        if (-not $monProc.HasExited) { Stop-Process -Id $monProc.Id -Force -ErrorAction SilentlyContinue }
        exit 1
    }
    Write-Host "monitor ready pid=$($monProc.Id) warmBatch=0 keep-alive full /api/gc"
}

function Get-Pct([System.Collections.Generic.List[int64]]$arr, [double]$p) {
    if ($arr.Count -lt 1) { return 0 }
    $sorted = $arr | Sort-Object
    $idx = [math]::Ceiling($p * $sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return [int64]$sorted[$idx]
}

function Read-HdrMs([System.Net.Http.HttpResponseMessage]$resp, [string]$name) {
    $vals = $null
    if ($resp.Headers.TryGetValues($name, [ref]$vals)) {
        $v = ($vals | Select-Object -First 1)
        $n = 0L
        if ([int64]::TryParse([string]$v, [ref]$n)) { return $n }
    }
    return $null
}

try {
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseCookies = $false
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(3)
    # 预热连接（keep-alive）
    try { $null = $client.GetAsync($Url).GetAwaiter().GetResult() } catch { }
    Start-Sleep -Milliseconds 100

    $lat = New-Object System.Collections.Generic.List[int64]
    $holds = New-Object System.Collections.Generic.List[int64]
    $sps = New-Object System.Collections.Generic.List[int64]
    $srvs = New-Object System.Collections.Generic.List[int64]
    $gcs = New-Object System.Collections.Generic.List[int64]
    $jsons = New-Object System.Collections.Generic.List[int64]
    $ok = 0
    $sawNursery = $false
    $sawFreelist = $false
    for ($i = 0; $i -lt $Samples; $i++) {
        try {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $resp = $client.GetAsync($Url).GetAwaiter().GetResult()
            $text = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            $sw.Stop()
            if (-not $resp.IsSuccessStatusCode) { throw "HTTP $($resp.StatusCode)" }
            $json = $text | ConvertFrom-Json
            if ($null -ne $json.nurseryBytes) { $sawNursery = $true }
            if ($null -ne $json.freelistHits) { $sawFreelist = $true }
            [void]$lat.Add([int64]$sw.ElapsedMilliseconds)
            $h = 0; if ($null -ne $json.lastCollectHoldMs) { $h = [int64]$json.lastCollectHoldMs }
            $sp = 0; if ($null -ne $json.lastStatsSafepointMs) { $sp = [int64]$json.lastStatsSafepointMs }
            $hdrSrv = Read-HdrMs $resp "X-Hao-Server-Ms"
            $hdrJson = Read-HdrMs $resp "X-Hao-Json-Ms"
            $hdrGc = Read-HdrMs $resp "X-Hao-GcStats-Ms"
            $srv = 0
            if ($null -ne $hdrSrv) { $srv = [int64]$hdrSrv }
            elseif ($null -ne $json.serverTotalMs) { $srv = [int64]$json.serverTotalMs }
            $gc = 0
            if ($null -ne $hdrGc) { $gc = [int64]$hdrGc }
            elseif ($null -ne $json.gcStatsMs) { $gc = [int64]$json.gcStatsMs }
            $jm = 0
            if ($null -ne $hdrJson) { $jm = [int64]$hdrJson }
            elseif ($null -ne $json.jsonMs) { $jm = [int64]$json.jsonMs }
            [void]$holds.Add($h)
            [void]$sps.Add($sp)
            [void]$srvs.Add($srv)
            [void]$gcs.Add($gc)
            [void]$jsons.Add($jm)
            $ok++
        } catch {
            Write-Host "WARN sample fail: $($_.Exception.Message)"
        }
        Start-Sleep -Milliseconds 30
    }
    $client.Dispose()
    if ($ok -lt [math]::Max(10, [int]($Samples / 2))) {
        Write-Host "FAIL GC_API_LATENCY too few samples ok=$ok/$Samples"
        exit 1
    }
    if (-not $sawNursery -or -not $sawFreelist) {
        Write-Host "FAIL GC_API_LATENCY lean body (need nurseryBytes+freelistHits); do not cut fields for green"
        exit 1
    }
    $p50 = Get-Pct $lat 0.50
    $p95 = Get-Pct $lat 0.95
    $holdP95 = Get-Pct $holds 0.95
    $spP95 = Get-Pct $sps 0.95
    $srvP95 = Get-Pct $srvs 0.95
    $gcP95 = Get-Pct $gcs 0.95
    $jsonP95 = Get-Pct $jsons 0.95
    Write-Host "GC_API_LATENCY samples=$ok wall p50=${p50} p95=${p95} | srvP95=$srvP95 gcP95=$gcP95 jsonP95=$jsonP95 holdP95=$holdP95 spP95=$spP95 (full snap)"

    if ($holdP95 -gt $MaxHoldP95Ms) {
        Write-Host "FAIL holdP95=$holdP95 > $MaxHoldP95Ms (lock tax)"
        exit 1
    }
    if ($spP95 -gt $MaxSafepointP95Ms) {
        Write-Host "FAIL safepointP95=$spP95 > $MaxSafepointP95Ms (STW park on stats)"
        exit 1
    }
    if ($srvP95 -gt $MaxServerP95Ms) {
        Write-Host "FAIL serverTotalP95=$srvP95 > $MaxServerP95Ms (handler/json/proc)"
        exit 1
    }
    if ($jsonP95 -gt $MaxJsonP95Ms) {
        Write-Host "FAIL jsonP95=$jsonP95 > $MaxJsonP95Ms (stringifyBean / double encode?)"
        exit 1
    }
    if ($p95 -gt 250) {
        Write-Host "FAIL wall p95=$p95 > 250 catastrophic"
        exit 1
    }
    if ($p95 -gt $MaxWallP95Ms) {
        Write-Host "WARN wall p95=$p95 > soft $MaxWallP95Ms (clientOh?) but server/hold/json OK"
    }
    Write-Host "GC_API_LATENCY_GATE OK"
    exit 0
}
finally {
    if ($null -ne $monProc -and -not $monProc.HasExited) {
        Stop-Process -Id $monProc.Id -Force -ErrorAction SilentlyContinue
    }
}
