$url = "http://127.0.0.1:18090/api/gc"
$totalHours = 24
$totalSeconds = $totalHours * 3600
$rps = 50

Write-Host "开始压测: $url"
Write-Host "持续 $totalHours 小时 | 每秒 $rps 请求`n"

# 记录脚本启动时间
$scriptStartTime = Get-Date
$endTime = $scriptStartTime.AddSeconds($totalSeconds)

$success = 0
$fail = 0
$consecutiveFail = 0
$maxConsecutiveFail = 10

# 保存最后一次成功的数据
$lastSuccessJson = $null
$lastSuccessTime = $null

$shouldStop = $false

while ((Get-Date) -lt $endTime -and -not $shouldStop) {
    for ($i = 0; $i -lt $rps; $i++) {
        $now = Get-Date -Format "yyyy-MM-dd HH:mm:ss.ffff"
        
        try {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $resp = Invoke-WebRequest -Uri $url -Method Get -TimeoutSec 2
            $sw.Stop()
            $ms = $sw.ElapsedMilliseconds

            $json = $resp.Content | ConvertFrom-Json
            $privateBytes = $json.privateBytes
            $privateMiB = [math]::Round($privateBytes / 1024 / 1024, 2)

            $success++
            $consecutiveFail = 0

            # 更新最后一次成功记录
            $lastSuccessJson = $json
            $lastSuccessTime = $now

            Write-Host "$now 请求成功 | 耗时: $ms ms | privateBytes: $privateMiB MiB"
        }
        catch {
            $fail++
            $consecutiveFail++
            Write-Host "$now 请求失败 | 连续失败: $consecutiveFail / $maxConsecutiveFail"

            if ($consecutiveFail -ge $maxConsecutiveFail) {
                $shouldStop = $true
                break
            }
        }
    }

    if (-not $shouldStop) {
        Start-Sleep -Seconds 1
    }
}

# 脚本结束时间
$scriptEndTime = Get-Date

# ===================== 最终输出 =====================
Write-Host "`n=================================================="
Write-Host "脚本开始运行时间：$($scriptStartTime.ToString('yyyy-MM-dd HH:mm:ss.ffff'))"
Write-Host "脚本结束运行时间：$($scriptEndTime.ToString('yyyy-MM-dd HH:mm:ss.ffff'))"
Write-Host "最后一次请求成功时间：$lastSuccessTime"

if ($lastSuccessJson -ne $null) {
    Write-Host "最后一次成功返回的 JSON 数据："
    Write-Host ($lastSuccessJson | ConvertTo-Json -Depth 5)
}
else {
    Write-Host "无任何一次成功请求"
}

Write-Host "`n=== 压测结束 ==="
Write-Host "总成功: $success"
Write-Host "总失败: $fail"