# B01/B02：G2-SYM 门禁 —— 符号登记仅经 src/sema/SymBind；IRGen 禁 syms_.declare*
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$fail = 0

$bindH = Join-Path $Root "src\sema\SymBind.h"
$bindCpp = Join-Path $Root "src\sema\SymBind.cpp"
if (-not (Test-Path $bindH)) { Write-Host "FAIL missing SymBind.h"; $fail++ } else { Write-Host "OK   SymBind.h" }
if (-not (Test-Path $bindCpp)) { Write-Host "FAIL missing SymBind.cpp"; $fail++ } else { Write-Host "OK   SymBind.cpp" }

$bindTxt = Get-Content -Raw $bindCpp
if ($bindTxt -notmatch 'bool symDeclare\(' -or $bindTxt -notmatch 'bool symDeclareGlobal\(') {
    Write-Host "FAIL SymBind.cpp missing symDeclare implementations"
    $fail++
} else { Write-Host "OK   SymBind implementations" }

$irgenDir = Join-Path $Root "src\irgen"
Get-ChildItem -Path $irgenDir -Recurse -Include *.cpp,*.h | ForEach-Object {
    $lines = Get-Content $_.FullName
    $ln = 0
    foreach ($line in $lines) {
        $ln++
        if ($line -match '^\s*//' -or $line -match '^\s*\*') { continue }
        if ($line -match 'syms_\.(declareGlobal|declare)\s*\(') {
            Write-Host ("FAIL {0}:{1}: {2}" -f $_.Name, $ln, $line.Trim())
            $fail++
        }
    }
}
if ($fail -eq 0) { Write-Host "OK   IRGen has zero syms_.declare*" }

$hao = Join-Path $Root "output\hao.exe"
$td = Join-Path $Root "target\test\sema_sym_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$src = Join-Path $td "sym.hao"
$exe = Join-Path $td "sym.exe"
@'
package main;
func add(a: Int, b: Int): Int { return a + b; }
func main() {
    var x = 1;
    fmt.println(add(x, 41) == 42);
}
'@ | Set-Content -Encoding utf8 $src
& $hao build $src -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL sema_sym sample build"
    $fail++
} else {
    $out = & $exe 2>&1 | Out-String
    if ($out -notmatch 'true') {
        Write-Host "FAIL sample run: $out"
        $fail++
    } else { Write-Host "OK   sample run true" }
}

if ($fail -gt 0) {
    Write-Host "SEMA_SYM_GATE FAIL ($fail)"
    exit 1
}
Write-Host "SEMA_SYM_GATE OK"
exit 0
