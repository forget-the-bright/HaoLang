# GC-HTTP-LAT 压测：HttpClient（弃用 IWR 作真值）+ 服务端分段 ms
# 口诀：clientOverhead≈wall → 客户端/连接；server≈wall 且 gcStats 大 → STW park；json 大 → 反射 JSON；hold 大 → 锁税
$url = "http://127.0.0.1:18090/api/gc"
$totalHours = 24
$totalSeconds = $totalHours * 3600
$rps = 50
$slowMs = 40
$windowSize = 200

Add-Type -AssemblyName System.Net.Http
$handler = [System.Net.Http.HttpClientHandler]::new()
$handler.UseCookies = $false
$client = [System.Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(2)

Write-Host "开始压测(HttpClient): $url"
Write-Host "持续 $totalHours 小时 | 每秒 $rps | SLOW>=${slowMs}ms | 看 srv/gc/json/clientOh`n"

$scriptStartTime = Get-Date
$endTime = $scriptStartTime.AddSeconds($totalSeconds)
$success = 0
$fail = 0
$consecutiveFail = 0
$maxConsecutiveFail = 10
$lastSuccessJson = $null
$lastSuccessTime = $null
$shouldStop = $false
$prevReq = $null
$lat = New-Object System.Collections.Generic.List[int64]

function Get-Pct([System.Collections.Generic.List[int64]]$arr, [double]$p) {
    if ($arr.Count -lt 1) { return 0 }
    $sorted = $arr | Sort-Object
    $idx = [math]::Ceiling($p * $sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return [int64]$sorted[$idx]
}
function Get-I64($obj, [string]$name) {
    if ($null -eq $obj) { return [int64]0 }
    $p = $obj.PSObject.Properties[$name]
    if ($null -eq $p -or $null -eq $p.Value) { return [int64]0 }
    return [int64]$p.Value
}

while ((Get-Date) -lt $endTime -and -not $shouldStop) {
    for ($i = 0; $i -lt $rps; $i++) {
        $now = Get-Date -Format "yyyy-MM-dd HH:mm:ss.ffff"
        try {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $resp = $client.GetAsync($url).GetAwaiter().GetResult()
            $text = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            $sw.Stop()
            $ms = [int64]$sw.ElapsedMilliseconds
            if (-not $resp.IsSuccessStatusCode) { throw "HTTP $($resp.StatusCode)" }
            $json = $text | ConvertFrom-Json
            $success++
            $consecutiveFail = 0
            $lastSuccessJson = $json
            $lastSuccessTime = $now
            [void]$lat.Add($ms)
            while ($lat.Count -gt $windowSize) { $lat.RemoveAt(0) }
            $p50 = Get-Pct $lat 0.50
            $p95 = Get-Pct $lat 0.95

            $srv = Get-I64 $json "serverTotalMs"
            $gcMs = Get-I64 $json "gcStatsMs"
            $jsonMs = Get-I64 $json "jsonMs"
            $procMs = Get-I64 $json "procMs"
            $hold = Get-I64 $json "lastCollectHoldMs"
            $sp = Get-I64 $json "lastStatsSafepointMs"
            $clientOh = $ms - $srv
            if ($clientOh -lt 0) { $clientOh = 0 }

            $interesting = $false
            if ($null -ne $prevReq) {
                $dCollect = (Get-I64 $json "collectCount") - (Get-I64 $prevReq "collectCount")
                $dInc = (Get-I64 $json "stwIncomplete") - (Get-I64 $prevReq "stwIncomplete")
                if ($dCollect -ne 0 -or $dInc -ne 0 -or $hold -ge 20 -or $sp -ge 10) { $interesting = $true }
            }
            if ($ms -ge $slowMs -or $interesting) {
                Write-Host ("$now REQΔ wall=${ms}ms srv=$srv gc=$gcMs json=$jsonMs proc=$procMs " +
                    "clientOh=$clientOh hold=$hold sp=$sp p50=$p50 p95=$p95 collect=$(Get-I64 $json 'collectCount')")
            }
            $prevReq = $json
        }
        catch {
            $fail++
            $consecutiveFail++
            Write-Host "$now 请求失败 | 连续失败: $consecutiveFail / $maxConsecutiveFail | $($_.Exception.Message)"
            if ($consecutiveFail -ge $maxConsecutiveFail) { $shouldStop = $true; break }
        }
    }
    if (-not $shouldStop) { Start-Sleep -Seconds 1 }
}

$client.Dispose()
$finalP50 = Get-Pct $lat 0.50
$finalP95 = Get-Pct $lat 0.95
Write-Host "`n=================================================="
Write-Host "结束 p50=$finalP50 p95=$finalP95 success=$success fail=$fail"
if ($lastSuccessJson -ne $null) {
    Write-Host ($lastSuccessJson | ConvertTo-Json -Depth 4)
}
