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
    'hao_array_get_ptr'
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

Write-Host "P5_SMOKE+IR_SYNC OK"
exit 0
