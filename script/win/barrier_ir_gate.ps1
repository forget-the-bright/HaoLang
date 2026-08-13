# T05：hao_gc_barrier IR 属性门禁（独立；禁全量）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$hao = Join-Path $Root "output\hao.exe"
$td = Join-Path $Root "target\test\barrier_ir_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$src = Join-Path $td "b.hao"
$ll = Join-Path $td "b.ll"
@'
package main;
class Box { var n: Object?; }
func main() {
    val b = new Box();
    b.n = new Box();
}
'@ | Set-Content -Encoding utf8 $src
& $hao emit $src -o $ll
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL BARRIER_IR emit"; exit 1 }
$txt = Get-Content -Raw $ll
$fail = 0
if ($txt -notmatch 'declare void @hao_gc_barrier\(ptr, ptr\) #1') {
    Write-Host "FAIL BARRIER_IR missing declare ... #1"
    $fail++
} else { Write-Host "OK   barrier declare #1" }
if ($txt -notmatch 'attributes #1 = \{[^}]*noinline') {
    Write-Host "FAIL BARRIER_IR attributes #1 missing noinline"
    $fail++
} else { Write-Host "OK   attributes #1 has noinline" }
if ($txt -notmatch 'attributes #1 = \{[^}]*memory\(readwrite\)') {
    Write-Host "FAIL BARRIER_IR attributes #1 must be memory(readwrite) (not argmem-only)"
    $fail++
} else { Write-Host "OK   attributes #1 has memory(readwrite)" }
if ($txt -match 'memory\(argmem:') {
    Write-Host "FAIL BARRIER_IR still has weak memory(argmem:...)"
    $fail++
} else { Write-Host "OK   no weak memory(argmem:)" }
if ($fail -gt 0) { Write-Host "BARRIER_IR_GATE FAIL"; exit 1 }
Write-Host "BARRIER_IR_GATE OK"
exit 0
