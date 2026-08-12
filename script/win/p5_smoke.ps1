# P5 stdlib + IR 同步独立冒烟（不跑 suite）
# 用法：仓库根目录 powershell -File script/win/p5_smoke.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not (Test-Path (Join-Path $root "VERSION"))) { $root = (Get-Location).Path }
Set-Location $root

$hao = Join-Path $root "output\hao.exe"
$dir = Join-Path $root "target\p5-smoke"
$src = Join-Path $dir "p5_lib_smoke.hao"
$exe = Join-Path $dir "p5_lib_smoke.exe"
$ll  = Join-Path $dir "p5_lib_smoke.ll"
$probeSrc = Join-Path $dir "p5_tostr_ir.hao"
$probeLl  = Join-Path $dir "p5_tostr_ir.ll"

if (-not (Test-Path $hao)) { throw "missing $hao" }
New-Item -ItemType Directory -Force -Path $dir | Out-Null

function Fail([string]$msg) {
    Write-Host "FAIL IR-SYNC $msg"
    exit 1
}

function Assert-NoMatch([string]$path, [string]$pattern, [string]$label) {
    $hits = Select-String -Path $path -Pattern $pattern
    if ($hits) {
        $hits | Select-Object -First 5 | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
        Fail "$label matched /$pattern/ in $path"
    }
    Write-Host "OK   IR no $label"
}

function Assert-HasMatch([string]$path, [string]$pattern, [string]$label) {
    $hits = Select-String -Path $path -Pattern $pattern
    if (-not $hits) { Fail "$label missing /$pattern/ in $path" }
    Write-Host "OK   IR has $label"
}

# ---- 1) 功能冒烟 ----
& $hao build $src -o $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 2) IR：TypeName.Class 必须 wrap；禁裸 classOfMeta(@T.meta) ----
& $hao emit $src -o $ll
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Assert-NoMatch $ll 'classOfMeta\(ptr @[A-Za-z0-9_$]+\.meta\)' "bare classOfMeta(@T.meta)"
Assert-HasMatch $ll 'hao_handle_wrap\(ptr @[A-Za-z0-9_$]+\.meta\)' "handle_wrap(@T.meta)"
Assert-HasMatch $ll 'reflect\$classOfMeta' "classOfMeta call"

# ---- 3) IR：已删 C 黑名单（P5 上移项）----
$deleted = @(
    'hao_str_substring',
    'hao_sync_lock',
    'hao_sync_unlock',
    'hao_time_format',
    'hao_time_field',
    'hao_parse_int',
    'hao_parse_long',
    'hao_parse_uint',
    'hao_parse_ulong',
    'hao_parse_bool',
    'hao_int_to_str',
    'hao_long_to_str',
    'hao_uint_to_str',
    'hao_ulong_to_str',
    'hao_bool_to_str',
    'hao_char_to_str',
    'hao_hash_str',
    'hao_reflect_is_assignable',
    'hao_str_byte_index_of',
    'hao_array_get_ptr',
    'hao_hash_float',
    'hao_hash_f32',
    'hao_println_int',
    'hao_println_bool',
    'hao_println_char',
    'hao_println_float',
    'hao_println_double',
    'hao_println_long',
    'hao_println_sbyte'
)
foreach ($sym in $deleted) {
    $pat = [regex]::Escape($sym)
    Assert-NoMatch $ll $pat "deleted $sym"
}

# ---- 4) IR：插值 toStr 正路径（Hao 方法，非已删 C）----
@"
package main;
func main() {
    fmt.println("i=" + 42 + " b=" + true + " c=" + 'A');
}
"@ | Set-Content -Encoding utf8 $probeSrc
& $hao emit $probeSrc -o $probeLl
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Assert-HasMatch $probeLl 'lang\$Integer\.toStr' "Integer.toStr"
Assert-HasMatch $probeLl 'lang\$Boolean\.toStr' "Boolean.toStr"
Assert-HasMatch $probeLl 'lang\$Character\.toStr' "Character.toStr"
Assert-NoMatch $probeLl 'hao_int_to_str|hao_bool_to_str|hao_char_to_str' "deleted toStr C"

# ---- 5) src/irgen 源码黑名单（防 declare 表回潮）----
$irFiles = Get-ChildItem -Path (Join-Path $root "src\irgen") -Recurse -File |
    Where-Object { $_.Extension -in '.cpp', '.h' }
foreach ($sym in $deleted) {
    $hits = $irFiles | Select-String -Pattern $sym -SimpleMatch
    if ($hits) {
        $hits | Select-Object -First 5 | ForEach-Object {
            Write-Host "  $($_.Path):$($_.LineNumber) $($_.Line.Trim())"
        }
        Fail "src/irgen still mentions $sym"
    }
}
Write-Host "OK   src/irgen free of deleted P5/P6 C symbols"

# ---- 6) hao_str_cstr 调用点白名单（仅定义 + ffi_dup）----
$cstrHits = Get-ChildItem -Path (Join-Path $root "stdlib") -Filter "*.c" -File |
    Select-String -Pattern 'hao_str_cstr\s*\('
foreach ($h in $cstrHits) {
    $ok = ($h.Filename -eq 'runtime_string.c') -or ($h.Filename -eq 'runtime_handle.c')
    if (-not $ok) {
        Write-Host "  $($h.Path):$($h.LineNumber) $($h.Line.Trim())"
        Fail "hao_str_cstr call outside whitelist: $($h.Filename)"
    }
}
# runtime_handle.c 仅允许在 hao_ffi_dup_cstr 内
$dupHits = Select-String -Path (Join-Path $root "stdlib\runtime_handle.c") -Pattern 'hao_str_cstr\s*\('
foreach ($h in $dupHits) {
    if ($h.LineNumber -lt 88 -or $h.LineNumber -gt 112) {
        Fail "hao_str_cstr in runtime_handle.c outside hao_ffi_dup_cstr (~L88-112)"
    }
}
Write-Host "OK   hao_str_cstr whitelist (string define + ffi_dup)"

# ---- 7) 源码无 hao_array_get_ptr（已删；.c/.cpp/.hao + irgen）----
$arrRoots = @(
    (Join-Path $root "stdlib"),
    (Join-Path $root "src")
)
$arrHits = foreach ($r in $arrRoots) {
    Get-ChildItem -Path $r -Recurse -File |
        Where-Object { $_.Extension -in '.c', '.cpp', '.hao', '.h' } |
        Select-String -Pattern 'hao_array_get_ptr' -SimpleMatch
}
$arrHits = @($arrHits | Where-Object {
    $_.Line -notmatch '已删|P6|get_obj|废弃|deleted'
})
if ($arrHits.Count -gt 0) {
    $arrHits | Select-Object -First 5 | ForEach-Object {
        Write-Host "  $($_.Path):$($_.LineNumber) $($_.Line.Trim())"
    }
    Fail "hao_array_get_ptr still present in code"
}
Write-Host "OK   hao_array_get_ptr deleted"

# ---- 8) hao_str_data 白名单 ----
$dataHits = Get-ChildItem -Path (Join-Path $root "stdlib") -Filter "*.c" -File |
    Select-String -Pattern 'hao_str_data\s*\('
foreach ($h in $dataHits) {
    $ok = $h.Filename -in @('runtime_string.c', 'runtime_fs.c', 'runtime_net.c')
    if (-not $ok) {
        Write-Host "  $($h.Path):$($h.LineNumber) $($h.Line.Trim())"
        Fail "hao_str_data outside whitelist: $($h.Filename)"
    }
}
Write-Host "OK   hao_str_data whitelist"

# ---- 9) channel 无 ptrtoint 别名 declare ----
$chHits = Select-String -Path (Join-Path $root "stdlib\src\channel\channel.hao") -Pattern 'hao_reflect_ptrtoint|hao_chan_str_to_i64'
if ($chHits) {
    Fail "channel still aliases ptrtoint"
}
Write-Host "OK   channel no ptrtoint alias"

# ---- 10) P7a 功能冒烟 ----
$p7a = Join-Path $root "target\p7-smoke\p7a_smoke.hao"
$p7aExe = Join-Path $root "target\p7-smoke\p7a_smoke.exe"
& $hao build $p7a -o $p7aExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $p7aExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 11) P7c：stdlib 无 strtof/strtod/%g 浮点路径 ----
$floatHits = Get-ChildItem -Path (Join-Path $root "stdlib") -Filter "runtime_*.c" -File |
    Select-String -Pattern 'strtof\s*\(|strtod\s*\(|snprintf\s*\([^;]*%g'
$floatHits = @($floatHits | Where-Object {
    $_.Filename -ne 'runtime_float.c' -or $_.Line -notmatch '禁 libc'
})
# runtime_float.c 仅允许注释提及；其它 runtime_*.c 禁止
$badFloat = @($floatHits | Where-Object {
    $_.Line -match 'strtof\s*\(|strtod\s*\(|snprintf\s*\([^;]*%g' -and $_.Line -notmatch '禁 libc|/\*'
})
if ($badFloat.Count -gt 0) {
    # 更严：任何非注释调用
    $calls = Get-ChildItem -Path (Join-Path $root "stdlib") -Filter "runtime_*.c" -File |
        Select-String -Pattern 'strtof\s*\(|strtod\s*\('
    $calls = @($calls | Where-Object { $_.Line -notmatch '^\s*\*|禁 libc' })
    if ($calls.Count -gt 0) {
        $calls | Select-Object -First 5 | ForEach-Object {
            Write-Host "  $($_.Path):$($_.LineNumber) $($_.Line.Trim())"
        }
        Fail "strtof/strtod still used in runtime"
    }
    $pct = Get-ChildItem -Path (Join-Path $root "stdlib") -Filter "runtime_*.c" -File |
        Select-String -Pattern 'snprintf\s*\([^;\n]*%g'
    $pct = @($pct | Where-Object { $_.Filename -ne 'runtime_float.c' })
    if ($pct.Count -gt 0) {
        $pct | Select-Object -First 5 | ForEach-Object {
            Write-Host "  $($_.Path):$($_.LineNumber) $($_.Line.Trim())"
        }
        Fail "snprintf %g still used outside runtime_float comments"
    }
}
Write-Host "OK   no libc float strto*/%g in runtime"

# ---- 12) P7d：ptrOf/objOf 业务白名单 ----
$ptrFiles = Get-ChildItem -Path (Join-Path $root "stdlib\src") -Recurse -Filter "*.hao" |
    Select-String -Pattern 'reflect\.ptrOf\(|reflect\.objOf\(|reflect\.ptrOfStr\(|reflect\.strOfInt\('
foreach ($h in $ptrFiles) {
    $rel = $h.Path.Substring($root.Length).TrimStart('\','/')
    $ok = $rel -match 'reflect[/\\]reflect\.hao$' -or
          $rel -match 'net[/\\]Mvc\.hao$' -or
          $rel -match 'net[/\\]Html\.hao$' -or
          $rel -match 'json[/\\]JSON\.hao$' -or
          $rel -match 'channel[/\\]channel\.hao$'
    if (-not $ok) {
        Write-Host "  ${rel}:$($h.LineNumber) $($h.Line.Trim())"
        Fail "ptrOf/objOf outside whitelist: $rel"
    }
}
Write-Host "OK   reflect ptrOf whitelist"

# ---- 13) 业务 .hao 禁 hao_array_*（布局 C 仅 runtime；JSON 已去 array 桥）----
$haoArrHits = Get-ChildItem -Path (Join-Path $root "stdlib\src") -Recurse -Filter "*.hao" |
    Select-String -Pattern 'hao_array_len|hao_array_get_obj|hao_array_get_ptr|hao_array_new|hao_array_push|hao_array_pop'
if ($haoArrHits) {
    $haoArrHits | Select-Object -First 8 | ForEach-Object {
        Write-Host "  $($_.Path):$($_.LineNumber) $($_.Line.Trim())"
    }
    Fail "stdlib .hao still calls hao_array_*"
}
Write-Host "OK   stdlib .hao free of hao_array_*"

# ---- 14) P8 Map/JSON 冒烟 ----
$p8 = Join-Path $root "target\p8-smoke\p8_all.hao"
$p8Exe = Join-Path $root "target\p8-smoke\p8_all.exe"
if (-not (Test-Path $p8)) { Fail "missing $p8" }
& $hao build $p8 -o $p8Exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $p8Exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "OK   p8 Map/JSON smoke"

# ---- 15) P9 OOP 全量收口冒烟矩阵 ----
$p9dir = Join-Path $root "target\p9-smoke"
$p9files = @(
    'oop_obj_assign',
    'oop_is_as_mono',
    'oop_is_as_raw',
    'oop_is_as_wildcard',
    'oop_iface_param',
    'oop_entryset_for',
    'oop_iface_extends',
    'oop_default_method',
    'oop_where',
    'oop_autoprop',
    'oop_list_arraylist',
    'oop_json_inherit'
)
foreach ($name in $p9files) {
    $src = Join-Path $p9dir "$name.hao"
    if (-not (Test-Path $src)) { Fail "missing $src" }
    & $hao run $src
    if ($LASTEXITCODE -ne 0) { Fail "p9 $name failed" }
    Write-Host "OK   p9 $name"
}
# where 违约束 / 双默认冲突须编译失败
foreach ($neg in @('oop_where_neg','oop_default_conflict')) {
    $src = Join-Path $p9dir "$neg.hao"
    $ll = Join-Path $p9dir "$neg.ll"
    if (-not (Test-Path $src)) { Fail "missing $src" }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $hao emit $src -o $ll 2>$null | Out-Null
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($ec -eq 0) { Fail "p9 $neg should reject" }
    Write-Host "OK   p9 $neg rejects"
}

# ---- 16) P10 类型系统 Java 对标（装箱 + 有界通配）----
$p10dir = Join-Path $root "target\p10-smoke"
$p10files = @(
    'box_implicit',
    'box_explicit_as',
    'box_generic',
    'wild_extends',
    'wild_super'
)
foreach ($name in $p10files) {
    $src = Join-Path $p10dir "$name.hao"
    if (-not (Test-Path $src)) { Fail "missing $src" }
    & $hao run $src
    if ($LASTEXITCODE -ne 0) { Fail "p10 $name failed" }
    Write-Host "OK   p10 $name"
}
# box_trap：运行时 panic（Object=10 后 as Long）
$trapSrc = Join-Path $p10dir "box_trap.hao"
$trapExe = Join-Path $p10dir "box_trap.exe"
if (-not (Test-Path $trapSrc)) { Fail "missing $trapSrc" }
& $hao build $trapSrc -o $trapExe
if ($LASTEXITCODE -ne 0) { Fail "p10 box_trap build failed" }
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $trapExe 2>$null | Out-Null
$trapEc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($trapEc -eq 0) { Fail "p10 box_trap should panic" }
Write-Host "OK   p10 box_trap panics"
# 编译拒绝：Int→Long 包装链式；? extends/? super 非法写
foreach ($neg in @('box_no_chain','wild_extends_neg','wild_super_neg')) {
    $src = Join-Path $p10dir "$neg.hao"
    $ll = Join-Path $p10dir "$neg.ll"
    if (-not (Test-Path $src)) { Fail "missing $src" }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $hao emit $src -o $ll 2>$null | Out-Null
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($ec -eq 0) { Fail "p10 $neg should reject" }
    Write-Host "OK   p10 $neg rejects"
}

# ---- 17) P7b～P7e 功能冒烟 ----
foreach ($name in @('p7b_smoke','p7c_smoke','p7d_smoke','p7e_smoke')) {
    $src = Join-Path $root "target\p7-smoke\$name.hao"
    $exe = Join-Path $root "target\p7-smoke\$name.exe"
    if (-not (Test-Path $src)) { Fail "missing $src" }
    & $hao build $src -o $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "P5_SMOKE+IR_SYNC+P7+P8+P9+P10 OK"
exit 0
