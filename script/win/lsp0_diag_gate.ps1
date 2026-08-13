# F01：TOOL-LSP0 — hao 诊断行可解析为 path:line:col（Problems 同源）
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$fail = 0

$diagTs = Join-Path $Root "tools\vscode_plugin\src\diagnostics.ts"
if (-not (Test-Path $diagTs)) {
    Write-Host "FAIL missing diagnostics.ts"
    $fail++
} else { Write-Host "OK   diagnostics.ts" }

$pkg = Get-Content (Join-Path $Root "tools\vscode_plugin\package.json") -Raw
if ($pkg -notmatch '"version":\s*"0\.1\.4"') {
    Write-Host "FAIL plugin semver not 0.1.4"
    $fail++
} else { Write-Host "OK   plugin 0.1.4" }
if ($pkg -notmatch 'haolang\.diagnose') {
    Write-Host "FAIL missing haolang.diagnose command"
    $fail++
} else { Write-Host "OK   diagnose command" }

$hao = Join-Path $Root "output\hao.exe"
$td = Join-Path $Root "target\test\lsp0_diag_gate"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$bad = Join-Path $td "bad.hao"
@'
package main;
func main() {
    var x: Int? = null;
    fmt.println(x + 1);
}
'@ | Set-Content -Encoding utf8 $bad
$out = ""
try {
    $out = & $hao build $bad -o (Join-Path $td "bad.exe") 2>&1 | ForEach-Object { "$_" } | Out-String
} catch {
    $out = $_.Exception.Message + "`n" + ($_ | Out-String)
}
# 合并 ErrorRecord 文本
if (-not $out) { $out = "" }
$combined = $out
if ($combined -notmatch 'bad\.hao:\d+:\d+:\s*错误') {
    Write-Host "FAIL hao diag line missing: $combined"
    $fail++
} else { Write-Host "OK   hao emits path:line:col 错误" }

# 解析器与插件同源正则
$re = [regex]'^(.+?):(\d+):(\d+):\s*(错误|警告|error|warning):\s*(.+)$'
$hit = 0
foreach ($line in ($combined -split "`n")) {
    if ($re.IsMatch($line.Trim())) { $hit++ }
}
if ($hit -lt 1) {
    Write-Host "FAIL parser hit=0"
    $fail++
} else { Write-Host "OK   diagnostic regex hits=$hit" }

Push-Location (Join-Path $Root "tools\vscode_plugin")
npm run compile 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL plugin compile"
    $fail++
} else { Write-Host "OK   plugin compile" }
Pop-Location

if ($fail -gt 0) { Write-Host "LSP0_DIAG_GATE FAIL ($fail)"; exit 1 }
Write-Host "LSP0_DIAG_GATE OK"
exit 0
