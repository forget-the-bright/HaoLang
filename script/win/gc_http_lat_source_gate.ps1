# GC-HTTP-LAT / v0.75.1 源码门禁：全量默认 + JsonBuf；禁瘦默认/双编/平方拼接回潮
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$fail = 0
$gc = Get-Content -Raw (Join-Path $Root "stdlib\runtime_gc.c")
$mvc = Get-Content -Raw (Join-Path $Root "stdlib\src\net\Mvc.hao")
$dash = Get-Content -Raw (Join-Path $Root "haolang-example\08-gc-monitor\monitor\Dashboard.hao")
$json = Get-Content -Raw (Join-Path $Root "stdlib\src\json\JSON.hao")

function Need($text, $pat, $msg) {
    if ($text -notmatch $pat) {
        Write-Host "FAIL GC_HTTP_LAT $msg"
        $script:fail++
    } else { Write-Host "OK   $msg" }
}

Need $gc "g_last_stats_safepoint_ms" "missing lastStatsSafepointMs"
Need $gc "s\[61\]\s*=\s*g_last_stats_safepoint_ms" "safepoint slot 61"
Need $dash "serverTotalMs" "missing serverTotalMs"
Need $dash "GcLatencyView" "GcLatencyView kept for /api/gc/lite"
Need $dash "/api/gc/lite" "missing /api/gc/lite"
Need $dash "threadCacheMs" "missing Toolhelp long TTL"
Need $dash "X-Hao-Json-Ms" "missing X-Hao-Json-Ms header"
Need $json "class JsonBuf" "missing JsonBuf"
Need $json "buf\.append" "stringifyBean must use JsonBuf.append"

# 默认 /api/gc 必须全量 GcSnapshot（禁 toLatencyView 作默认）
$apiGc = [regex]::Match($dash, '(?s)func apiGc\(req: HttpRequest\): HttpResponse \{.*?return r;\s*\}')
if (-not $apiGc.Success) {
    Write-Host "FAIL GC_HTTP_LAT cannot locate apiGc"
    $fail++
} elseif ($apiGc.Value -match 'toLatencyView') {
    Write-Host "FAIL GC_HTTP_LAT default /api/gc still uses toLatencyView (lean)"
    $fail++
} elseif ($apiGc.Value -notmatch 'snap\.toJson\(\)') {
    Write-Host "FAIL GC_HTTP_LAT default /api/gc must use snap.toJson() (not reflect stringify)"
    $fail++
} else {
    Write-Host "OK   default /api/gc uses hand-written snap.toJson()"
}
Need $dash "func toJson\(\)" "missing GcSnapshot.toJson"

# stringifyBean 热路径禁 r = r +
$m = [regex]::Match($json, '(?s)static func stringifyBean\(.*?return buf\.finish\(\);\s*\}')
if (-not $m.Success) {
    Write-Host "FAIL GC_HTTP_LAT cannot locate stringifyBean+JsonBuf body"
    $fail++
} elseif ($m.Value -match 'r\s*=\s*r\s*\+') {
    Write-Host "FAIL GC_HTTP_LAT stringifyBean quadratic r=r+ regression"
    $fail++
} else {
    Write-Host "OK   stringifyBean no r=r+"
}

# 禁止 /api/ 一律 close
if ($mvc -match 'startsWith\("/api/"\)\s*\{\s*useKeep\s*=\s*false') {
    Write-Host "FAIL GC_HTTP_LAT /api still force-close without env"
    $fail++
} else {
    Write-Host "OK   /api keep-alive unless HAO_HTTP_API_SHORT"
}
Need $mvc "HAO_HTTP_API_SHORT" "missing HAO_HTTP_API_SHORT gate"

if ($fail -gt 0) {
    Write-Host "GC_HTTP_LAT_SOURCE_GATE FAIL ($fail)"
    exit 1
}
Write-Host "GC_HTTP_LAT_SOURCE_GATE OK"
exit 0
