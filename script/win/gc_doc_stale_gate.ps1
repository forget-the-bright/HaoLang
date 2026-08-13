# A01: GC doc stale gate — thin wrapper over Python (UTF-8 paths)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$py = Join-Path $PSScriptRoot "gc_doc_stale_gate.py"
python $py
exit $LASTEXITCODE
