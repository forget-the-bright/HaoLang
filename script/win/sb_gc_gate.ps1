# v0.78：SB/ArrayList 跨 ensureCapacity + GC；禁 str_len UAF 回归
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = ".\output\hao.exe"
if (-not (Test-Path $hao)) { throw "缺少 output/hao.exe" }

$outDir = "target\test\sb_gc"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function RunSmoke([string]$src, [string]$name, [string]$expect) {
    $exe = Join-Path $outDir "$name.exe"
    & $hao build $src -o $exe
    if ($LASTEXITCODE -ne 0) { throw "BUILD_FAIL $name" }
    $o = & $exe
    if ($o -notmatch [regex]::Escape($expect)) {
        throw "FAIL $name got: $o"
    }
    Write-Host "OK   $name"
}

RunSmoke "test\sb_append_gc_smoke.hao" "sb_append_gc" "sb_append_gc_smoke OK"
RunSmoke "test\arraylist_add_gc_smoke.hao" "arraylist_add_gc" "arraylist_add_gc_smoke OK"

# 禁恢复 hao_str_byte_arr；禁 payload 半套；禁死 str_len
$rs = Get-Content -Raw "stdlib\runtime_string.c"
if ($rs -match "hao_str_byte_arr") {
    throw "FAIL hao_str_byte_arr must be deleted"
}
if ($rs -match "hao_str_payload") {
    throw "FAIL hao_str_payload must be deleted"
}
if ($rs -match "hao_str_len\s*\(") {
    throw "FAIL hao_str_len must be deleted"
}
Write-Host "OK   no payload + no byte_arr + no str_len"

Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue
Write-Host "SB_GC_GATE OK"
exit 0
