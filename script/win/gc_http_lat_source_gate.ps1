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
Need $json "lang\.StringBuilder" "JsonBuf must use StringBuilder"
Need $json "quoteIntoSb" "missing quoteIntoSb"
Need $json "_jsonTryWrite|hao_json_try_write" "missing json try_write"
Need $json "buf\.append|stringifyBeanInto" "stringifyBean must use JsonBuf"

# 默认 /api/gc 必须全量 GcSnapshot（禁 toLatencyView 作默认）
$apiGc = [regex]::Match($dash, '(?s)func apiGc\(req: HttpRequest\): HttpResponse \{.*?return r;\s*\}')
if (-not $apiGc.Success) {
    Write-Host "FAIL GC_HTTP_LAT cannot locate apiGc"
    $fail++
} elseif ($apiGc.Value -match 'toLatencyView') {
    Write-Host "FAIL GC_HTTP_LAT default /api/gc still uses toLatencyView (lean)"
    $fail++
} elseif ($apiGc.Value -match 'snap\.toJson\(') {
    Write-Host "FAIL GC_HTTP_LAT default still hand-written toJson (symptom bypass)"
    $fail++
} elseif ($apiGc.Value -notmatch 'toJSONString\(snap\)') {
    Write-Host "FAIL GC_HTTP_LAT default /api/gc must use JSON.toJSONString(snap)"
    $fail++
} else {
    Write-Host "OK   default /api/gc uses toJSONString (Bean / `$jsonWrite)"
}
Need $json "class JsonBeanInfo" "missing JsonBeanInfo cache"
Need $json "getFieldAt" "stringifyBean must use getFieldAt"
Need $json "beanInfoOf" "missing beanInfoOf"
Need $json "stringifyAtInto" "missing stringifyAtInto for `$jsonWrite dispatch"

$bb = Get-Content -Raw (Join-Path $Root "stdlib\src\net\ByteBuf.hao")
$bbCode = [regex]::Replace($bb, '(?m)//.*$', '')
if ($bbCode -match 'data\s*=\s*data\s*\+') {
    Write-Host "FAIL GC_HTTP_LAT ByteBuf still fake buffer data=data+"
    $fail++
} else {
    Write-Host "OK   ByteBuf not data=data+"
}
$sbPath = Join-Path $Root "stdlib\src\lang\StringBuilder.hao"
if (-not (Test-Path $sbPath)) {
    Write-Host "FAIL GC_HTTP_LAT missing lang.StringBuilder"
    $fail++
} else {
    Write-Host "OK   lang.StringBuilder present"
}

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

# v0.79.3：writeResponse 须 SB 组头 + 分开发送；禁 out=out+body
$http = Get-Content -Raw (Join-Path $Root "stdlib\src\net\Http.hao")
$wr = [regex]::Match($http, '(?s)static func writeResponse\(client: Socket, resp: HttpResponse, keepAlive: Bool\) \{.*?resp\.writeMs\s*=')
if (-not $wr.Success) {
    Write-Host "FAIL GC_HTTP_LAT cannot locate writeResponse+writeMs"
    $fail++
} else {
    $body = $wr.Value
    $code = [regex]::Replace($body, '(?m)//.*$', '')
    if ($code -notmatch 'StringBuilder') {
        Write-Host "FAIL GC_HTTP_LAT writeResponse must use StringBuilder"
        $fail++
    } else { Write-Host "OK   writeResponse uses StringBuilder" }
    if ($code -match 'out\s*=\s*out\s*\+') {
        Write-Host "FAIL GC_HTTP_LAT writeResponse still out=out+"
        $fail++
    } else { Write-Host "OK   writeResponse no out=out+" }
    if ($code -notmatch 'client\.send\(head\)' -or $code -notmatch 'client\.send\(resp\.body\)') {
        Write-Host "FAIL GC_HTTP_LAT writeResponse must send(head) then send(body)"
        $fail++
    } else { Write-Host "OK   writeResponse two-send head/body" }
}

# v0.79.4：decodeChunkedBody 须 SB；禁 out=out+
$dc = [regex]::Match($http, '(?s)static func decodeChunkedBody\(rawBody: String\): String \{.*?return .*\.toString\(\);\s*\}')
if (-not $dc.Success) {
    Write-Host "FAIL GC_HTTP_LAT cannot locate decodeChunkedBody"
    $fail++
} else {
    $dcCode = [regex]::Replace($dc.Value, '(?m)//.*$', '')
    if ($dcCode -notmatch 'StringBuilder') {
        Write-Host "FAIL GC_HTTP_LAT decodeChunkedBody must use StringBuilder"
        $fail++
    } else { Write-Host "OK   decodeChunkedBody uses StringBuilder" }
    if ($dcCode -match 'out\s*=\s*out\s*\+') {
        Write-Host "FAIL GC_HTTP_LAT decodeChunkedBody still out=out+"
        $fail++
    } else { Write-Host "OK   decodeChunkedBody no out=out+" }
}

# v0.79.4：ByteBuf 读路径禁全量 toString 再 slice
if ($bbCode -match 'val\s+all\s*=\s*this\.sb\.toString\(\)' -or $bbCode -match '_bbSlice\(all') {
    Write-Host "FAIL GC_HTTP_LAT ByteBuf still toString+slice on read path"
    $fail++
} else {
    Write-Host "OK   ByteBuf no full toString+slice"
}
Need $bb "byteSlice" "ByteBuf must use StringBuilder.byteSlice"

if ($fail -gt 0) {
    Write-Host "GC_HTTP_LAT_SOURCE_GATE FAIL ($fail)"
    exit 1
}
Write-Host "GC_HTTP_LAT_SOURCE_GATE OK"
exit 0
