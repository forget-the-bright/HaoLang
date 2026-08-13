# T02/D05：stdlib 资源藏针回归门禁（独立；禁全量套件）
# 用法（仓库根）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\handle_resource_gate.ps1
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$fail = 0
$srcRoot = Join-Path $Root "stdlib\src"
$files = Get-ChildItem -Path $srcRoot -Recurse -Filter *.hao

# 禁止：hao_pool_* 仍返回/接收 Long
$poolLong = Select-String -Path $files.FullName -Pattern 'hao_pool_\w+\([^)]*\): Long|hao_pool_submit\(pool: Long|hao_pool_new\([^)]*\): Long'
if ($poolLong) {
    Write-Host "FAIL HANDLE_GATE pool still Long:"
    $poolLong | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    $fail++
} else {
    Write-Host "OK   no hao_pool_*: Long"
}

# 禁止：ThreadPool.pool 仍为 Long
$poolField = Select-String -Path (Join-Path $srcRoot "thread\thread.hao") -Pattern 'var pool:\s*Long'
if ($poolField) {
    Write-Host "FAIL HANDLE_GATE ThreadPool.pool is Long"
    $fail++
} else {
    Write-Host "OK   ThreadPool.pool not Long"
}

# 禁止：资源嫌疑字段用 Long/Long? 藏针（含 state/meta/conn 等）
$longBanNames = 'fd|handle|pool|file|sock|socket|pattern|prog|chan|stream|mtx|cell|conn|listener|thr|state|meta'
$resourceLong = Select-String -Path $files.FullName -Pattern ("var\s+($longBanNames)\s*:\s*Long\??\b")
if ($resourceLong) {
    Write-Host "FAIL HANDLE_GATE resource field Long:"
    $resourceLong | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    $fail++
} else {
    Write-Host "OK   no resource field Long hide"
}

# 短名 h：仅资源包禁 Long（hash FNV 的 h: Long 合法）
$resPkgs = @(
    (Join-Path $srcRoot "channel"),
    (Join-Path $srcRoot "thread"),
    (Join-Path $srcRoot "net"),
    (Join-Path $srcRoot "os"),
    (Join-Path $srcRoot "regex"),
    (Join-Path $srcRoot "sync"),
    (Join-Path $srcRoot "fs")
) | Where-Object { Test-Path $_ }
$hFiles = @()
foreach ($p in $resPkgs) {
    $hFiles += @(Get-ChildItem -Path $p -Recurse -Filter *.hao -ErrorAction SilentlyContinue)
}
if ($hFiles.Count -gt 0) {
    $hLong = Select-String -Path $hFiles.FullName -Pattern 'var\s+h\s*:\s*Long\??\b'
    if ($hLong) {
        Write-Host "FAIL HANDLE_GATE short name h: Long in resource pkg:"
        $hLong | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
        $fail++
    } else {
        Write-Host "OK   no var h: Long in resource pkgs"
    }
}

# 底层 C 资源槽：必须 NativeHandle（不含 sock→Socket / pool→ThreadPool / state→Int 等 Hao 包装）
$mustNhNames = 'fd|handle|pattern|prog|stream|mtx|cell|chan|file|socket'
$badNh = @()
foreach ($f in $files) {
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $n++
        if ($line -match ("var\s+($mustNhNames)\s*:\s*(\S+)")) {
            $ty = $Matches[2].TrimEnd(';')
            if ($ty -notmatch '^NativeHandle\??$') {
                $badNh += "  $($f.FullName):${n}: $($line.Trim()) (type=$ty)"
            }
        }
    }
}
foreach ($f in $hFiles) {
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $n++
        if ($line -match 'var\s+h\s*:\s*(\S+)') {
            $ty = $Matches[1].TrimEnd(';')
            if ($ty -notmatch '^NativeHandle\??$') {
                $badNh += "  $($f.FullName):${n}: $($line.Trim()) (type=$ty)"
            }
        }
    }
}
# ThreadPool.pool 特判：必须 NativeHandle
$tp = Join-Path $srcRoot "thread\thread.hao"
if (Test-Path $tp) {
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $tp)) {
        $n++
        if ($line -match 'var\s+pool\s*:\s*(\S+)') {
            $ty = $Matches[1].TrimEnd(';')
            if ($ty -notmatch '^NativeHandle\??$') {
                $badNh += "  ${tp}:${n}: $($line.Trim()) (type=$ty)"
            }
        }
    }
}
if ($badNh.Count -gt 0) {
    Write-Host "FAIL HANDLE_GATE raw resource slot must be NativeHandle:"
    $badNh | ForEach-Object { Write-Host $_ }
    $fail++
} else {
    Write-Host "OK   raw resource slots are NativeHandle"
}

# 必须：hao_pool_new 返回 NativeHandle
$poolNh = Select-String -Path (Join-Path $srcRoot "thread\thread.hao") -Pattern 'hao_pool_new\(n: Int\): NativeHandle'
if (-not $poolNh) {
    Write-Host "FAIL HANDLE_GATE hao_pool_new must return NativeHandle"
    $fail++
} else {
    Write-Host "OK   hao_pool_new: NativeHandle"
}

if ($fail -gt 0) {
    Write-Host "HANDLE_RESOURCE_GATE FAIL ($fail)"
    exit 1
}
Write-Host "HANDLE_RESOURCE_GATE OK"
exit 0
