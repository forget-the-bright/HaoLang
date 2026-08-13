# T08：G2 Sema 首刀门禁 —— resolveType 在 src/sema；IRGen 仅薄封装（禁全量）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$fail = 0

$semaCpp = Join-Path $Root "src\sema\TypeResolve.cpp"
$semaH = Join-Path $Root "src\sema\TypeResolve.h"
$irgenCpp = Join-Path $Root "src\irgen\IRGen.cpp"

if (-not (Test-Path $semaCpp)) {
    Write-Host "FAIL missing src/sema/TypeResolve.cpp"
    $fail++
} else { Write-Host "OK   TypeResolve.cpp exists" }

if (-not (Test-Path $semaH)) {
    Write-Host "FAIL missing src/sema/TypeResolve.h"
    $fail++
} else { Write-Host "OK   TypeResolve.h exists" }

$semaTxt = Get-Content -Raw $semaCpp
if ($semaTxt -notmatch '(?m)^TypePtr resolveType\(') {
    Write-Host "FAIL TypeResolve.cpp missing TypePtr resolveType("
    $fail++
} else { Write-Host "OK   resolveType implementation in sema" }

$irTxt = Get-Content -Raw $irgenCpp
if ($irTxt -match '预置 Action/Func 泛型函数类型别名') {
    Write-Host "FAIL IRGen.cpp still inlines Action/Func resolveType body"
    $fail++
} else { Write-Host "OK   IRGen.cpp has no inlined Action/Func resolve body" }

if ($irTxt -notmatch 'hao::resolveType\(typeResolveEnv') {
    Write-Host "FAIL IRGen.cpp missing thin hao::resolveType call"
    $fail++
} else { Write-Host "OK   IRGen.cpp thin-calls hao::resolveType" }

$hao = Join-Path $Root "output\hao.exe"
$td = Join-Path $Root "target\test\sema_cut_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$src = Join-Path $td "cut.hao"
$exe = Join-Path $td "cut.exe"
@'
package main;
func id(x: Int): Int { return x; }
func main() {
    fmt.println(id(41) + 1 == 42);
}
'@ | Set-Content -Encoding utf8 $src
& $hao build $src -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL sema_cut sample build"
    $fail++
} else {
    Write-Host "OK   sample build"
    $out = & $exe 2>&1 | Out-String
    if ($out -notmatch 'true') {
        Write-Host "FAIL sample run: $out"
        $fail++
    } else { Write-Host "OK   sample run true" }
}

if ($fail -gt 0) {
    Write-Host "SEMA_CUT_GATE FAIL ($fail)"
    exit 1
}
Write-Host "SEMA_CUT_GATE OK"
exit 0
