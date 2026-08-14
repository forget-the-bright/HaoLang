# v0.79.2：String.value 字段硬对齐；Ban 薄 FFI / payload / appendUtf8
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

$kill = "hao_str_payload|hao_str_get_bytes|hao_str_cp_len|hao_str_set_byte_len|hao_str_byte_slice|hao_copy_cstr_to_arr|appendUtf8|hao_str_from_byte_arr"

$sb = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\StringBuilder.hao")
NeedText $sb "var count:\s*Int" "SB has count field"
NeedText $sb "var value:\s*\[Byte\]" "SB has value buffer"
NeedText $sb "s\.value" "SB append reads s.value"
BanCode $sb $kill "SB no thin string FFI"

$al = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\List.hao")
NeedText $al "elementData" "ArrayList elementData"
NeedText $al "var count:\s*Int" "ArrayList count"
BanCode $al "items\s*\+=" "ArrayList no items+="

$arr = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\Arrays.hao")
NeedText $arr "func arraycopy" "Arrays.arraycopy present"
NeedText $arr "hao_arraycopy" "Arrays uses hao_arraycopy"

$rt = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\runtime_array.c")
NeedText $rt "hao_arraycopy" "C hao_arraycopy"
NeedText $rt "hao_array_from_cstr" "C hao_array_from_cstr"
BanCode $rt "hao_array_push|hao_array_pop|hao_array_ensure_cap|hao_array_set_len|hao_array_append_" "no push/pop/v0.76 C"

$native = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\native.hao")
NeedText $native "hao_object_new" "native has object_new"
BanCode $native $kill "native no thin string FFI"
BanCode $native "hao_str_byte_len|hao_str_from_cstr|hao_str_byte_arr" "native no str business extern"
BanCode $native "hao_float_to_str|hao_double_to_str|hao_parse_float|hao_parse_double" "native no float business"
BanCode $native "hao_str_len|hao_str_concat|hao_str_char_at|hao_str_byte_of_cp" "native no str algorithm"
BanCode $native "hao_f64_to_i64|hao_str_get_cp_len|hao_str_set_cp_len" "native no bandaid FFI"

$obj = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\object\Object.hao")
BanCode $obj "hao_object_hashCode|hao_object_equals|hao_object_toString" "Object defaults in Hao"
NeedText $obj "ptrOf\(this\)" "Object uses ptrOf"
NeedText $obj "StringBuilder.withCapacity" "ptrAngle uses SB"
BanCode $obj "Character\.toStr" "ptrAngle no Character.toStr+"

$flt = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\Float.hao")
BanCode $flt "hao_float_to_str|hao_parse_float" "Float toStr/parse Hao"
NeedText $flt "func appendTo" "Float.appendTo"
$dbl = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\Double.hao")
BanCode $dbl "hao_double_to_str|hao_parse_double|hao_f64_to_i64" "Double no C/fmt bandaid"
NeedText $dbl "func appendTo" "Double.appendTo"

$str = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\lang\String.hao")
NeedText $str "fromUtf8" "String.fromUtf8"
NeedText $str "s\.value" "String reads s.value"
NeedText $str "s\.cpLen" "String reads s.cpLen"
NeedText $str "StringBuilder.withCapacity" "String.concat via SB"
NeedText $str "Character.utf8CpCount" "String.codePointLen Hao"
BanCode $str $kill "String no thin string FFI"
BanCode $str "hao_str_concat|hao_str_len|hao_str_char_at|hao_str_byte_of_cp" "String no C str algo"

$json = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\json\JSON.hao")
NeedText $json "s\.value" "quote uses s.value"
BanCode $json $kill "JSON no thin string FFI"

$irc = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IRGenClass.cpp")
NeedText $irc "emitStringFromLiteral" "jsonWrite/literals via emitStringFromLiteral"
NeedText $irc "Double\.appendTo|Float\.appendTo" "jsonWrite Float/Double appendTo"
BanCode $irc "appendUtf8|@hao_str_from_cstr" "jsonWrite no appendUtf8/from_cstr"
BanCode $irc '@lang\$Float\.toStr|@lang\$Double\.toStr' "jsonWrite no toStr+append"

$refl = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\reflect\reflect.hao")
NeedText $refl "hao_reflect_field_bits_at" "reflect field bits trampoline"
BanCode $refl "hao_reflect_field_get_at|hao_reflect_field_get\b" "reflect no C field_get stringify"

$rtc = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\runtime_reflect.c")
BanCode $rtc "field_value_to_str" "C no field_value_to_str"
BanCode $rtc "hao_reflect_bool_val|hao_reflect_val_bool" "C no dead bool export"

$rts = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\runtime_string.c")
BanCode $rts "hao_str_payload|hao_str_get_bytes|hao_str_cp_len|hao_str_set_byte_len|hao_str_byte_slice|hao_copy_cstr_to_arr" "C no dead thin string exports"
BanCode $rts "hao_str_len\s*\(|hao_str_concat\s*\(|hao_str_char_at\s*\(|hao_str_byte_of_cp\s*\(" "C no dead str algo"
BanCode $rts "hao_str_get_cp_len|hao_str_set_cp_len" "C no cp_len get/set"

$rtf = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\runtime_float.c")
BanCode $rtf "hao_fmt_double|hao_f64_to_i64" "C no fmt_double/f64_to_i64"

$emit = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IREmitter.h")
BanCode $emit "hao_array_push|hao_array_pop|hao_array_cap" "IR no push/pop/cap declare"
BanCode $emit "hao_float_to_str|hao_double_to_str" "IR no float_to_str declare"
NeedText $emit "hao_array_from_cstr" "IR declares array_from_cstr"

$irv = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IRGenValue.cpp")
BanCode $irv "@hao_float_to_str|@hao_double_to_str" "IRGenValue uses Float/Double.toStr"

$hm = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\collections\HashMap.hao")
BanCode $hm "table\s*\+=\s*null" "HashMap no table+=null"

$ir = Get-Content -Raw -Encoding utf8 (Join-Path $Root "src\irgen\IRGenExpr.cpp")
NeedText $ir 'field == "capacity"' "IRGen rejects array.capacity"
NeedText $ir "stringInternalSlot|canAccessStringInternals" "IRGen String value/cpLen fields"
BanCode $ir "@hao_array_push|@hao_array_pop" "IRGen no array push/pop calls"
$irCode = StripComments $ir
$nGuide = ([regex]::Matches($irCode, 'collections\.ArrayList')).Count
if ($nGuide -lt 4) {
    Write-Host "FAIL SB_GATE IRGen array+=/pop reject messages ($nGuide)"
    $script:fail++
} else { Write-Host "OK   IRGen rejects array+=/pop ($nGuide guides)" }

# HTTP 出站：writeResponse 禁 String+ 拼 body（v0.79.3 wall 根因）
$http = Get-Content -Raw -Encoding utf8 (Join-Path $Root "stdlib\src\net\Http.hao")
$wr = [regex]::Match($http, '(?s)static func writeResponse\(client: Socket, resp: HttpResponse, keepAlive: Bool\) \{.*?resp\.writeMs')
if (-not $wr.Success) {
    Write-Host "FAIL SB_GATE writeResponse+writeMs missing"
    $script:fail++
} else {
    NeedText $wr.Value "StringBuilder" "writeResponse uses StringBuilder"
    BanCode $wr.Value "out\s*=\s*out\s*\+" "writeResponse no out=out+"
    NeedText $wr.Value "client\.send\(resp\.body\)" "writeResponse send body separate"
}

# 非 lang/json 禁止读内建 String 内部字段写法（粗检：包级 .value/.cpLen 业务）
foreach ($rel in @(
    "stdlib\src\collections",
    "stdlib\src\net",
    "stdlib\src\http",
    "stdlib\src\reflect",
    "stdlib\src\object",
    "stdlib\src\os",
    "stdlib\src\regex"
)) {
    $dir = Join-Path $Root $rel
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem -Recurse -Filter *.hao $dir | ForEach-Object {
        $t = Get-Content -Raw -Encoding utf8 $_.FullName
        BanCode (StripComments $t) $kill "kill-list only lang ($($_.Name))"
        BanCode (StripComments $t) "hao_fs_read_str|hao_net_recv\b|hao_net_udp_recvfrom\b" "IO via bytes ($($_.Name))"
    }
}

if ($fail -gt 0) {
    Write-Host "STRINGBUILDER_GATE FAIL ($fail)"
    exit 1
}
Write-Host "STRINGBUILDER_GATE OK"
exit 0
