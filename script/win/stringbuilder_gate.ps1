# v0.78：定长数组 + arraycopy；禁 C push/pop/byte_arr 与 v0.76 业务 append
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$fail = 0

function NeedText([string]$text, [string]$pat, [string]$msg) {
    if ($text -notmatch $pat) {
        Write-Host "FAIL SB_GATE $msg"
        $script:fail++
    } else { Write-Host "OK   $msg" }
}
function StripComments([string]$text) {
    return [regex]::Replace($text, '(?m)//.*$', '')
}
function BanCode([string]$text, [string]$pat, [string]$msg) {
    $code = StripComments $text
    if ($code -match $pat) {
        Write-Host "FAIL SB_GATE $msg"
        $script:fail++
    } else { Write-Host "OK   $msg" }
}

$sb = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\StringBuilder.hao")
NeedText $sb "var count:\s*Int" "SB has count field"
NeedText $sb "var value:\s*\[Byte\]" "SB has value buffer"
NeedText $sb "hao_str_get_bytes" "SB append uses get_bytes first"
BanCode $sb "hao_str_byte_arr|hao_array_ensure_cap|hao_array_set_len|hao_array_append_" "SB no alias/C append"

$al = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\List.hao")
NeedText $al "elementData" "ArrayList elementData"
NeedText $al "var count:\s*Int" "ArrayList count"
BanCode $al "items\s*\+=" "ArrayList no items+="

$arr = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\Arrays.hao")
NeedText $arr "func arraycopy" "Arrays.arraycopy present"
NeedText $arr "hao_arraycopy" "Arrays uses hao_arraycopy"

$rt = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\runtime_array.c")
NeedText $rt "hao_arraycopy" "C hao_arraycopy"
BanCode $rt "hao_array_push|hao_array_pop|hao_array_ensure_cap|hao_array_set_len|hao_array_append_" "no push/pop/v0.76 C"

$native = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\native.hao")
BanCode $native "hao_str_byte_arr" "native no hao_str_byte_arr"

$hm = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\HashMap.hao")
BanCode $hm "table\s*\+=\s*null" "HashMap no table+=null"

$ir = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IRGenExpr.cpp")
NeedText $ir 'field == "capacity"' "IRGen rejects array.capacity"
BanCode $ir "@hao_array_push|@hao_array_pop" "IRGen no array push/pop calls"
$irCode = StripComments $ir
$nGuide = ([regex]::Matches($irCode, 'collections\.ArrayList')).Count
if ($nGuide -lt 4) {
    Write-Host "FAIL SB_GATE IRGen array+=/pop reject messages ($nGuide)"
    $script:fail++
} else { Write-Host "OK   IRGen rejects array+=/pop ($nGuide guides)" }

$emit = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IREmitter.h")
BanCode $emit "hao_array_push|hao_array_pop|hao_array_cap" "IR no push/pop/cap declare"

if ($fail -gt 0) {
    Write-Host "STRINGBUILDER_GATE FAIL ($fail)"
    exit 1
}
Write-Host "STRINGBUILDER_GATE OK"
exit 0
