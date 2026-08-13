# T04：Unwind↔GC 全量冒烟门禁（独立；禁全量套件）
# 用法（仓库根）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\unwind_full_gate.ps1
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { throw "missing $hao" }

$td = Join-Path $Root "target\test\unwind_full_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null

$smokes = Get-ChildItem (Join-Path $Root "test") -Filter "gc_try_*_root_smoke.hao" | Sort-Object Name
if ($smokes.Count -lt 20) {
    Write-Host "FAIL UNWIND_GATE expected >=20 gc_try_*_root_smoke, got $($smokes.Count)"
    exit 1
}

$fail = 0
foreach ($f in $smokes) {
    $exe = Join-Path $td ($f.BaseName + ".exe")
    & $hao build $f.FullName -o $exe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL BUILD $($f.Name)"
        $fail++
        continue
    }
    $out = & $exe 2>&1 | ForEach-Object { "$_" }
    $joined = ($out -join "`n")
    if ($joined -match "false" -or $LASTEXITCODE -ne 0) {
        Write-Host "FAIL RUN $($f.Name) exit=$LASTEXITCODE"
        Write-Host $joined
        $fail++
    } else {
        Write-Host "OK   $($f.Name)"
    }
}

if ($fail -gt 0) {
    Write-Host "UNWIND_FULL_GATE FAIL ($fail)"
    exit 1
}
Write-Host "UNWIND_FULL_GATE OK ($($smokes.Count) smokes)"
exit 0
