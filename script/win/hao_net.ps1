# ============================================================
#  HaoLang 网络/代理公共函数（供 setup / fetch_* 点源）
# ------------------------------------------------------------
#  GitHub 下载顺序：
#    1) 直连
#    2) ghfast 镜像（默认 https://ghfast.top/原URL，可用 HAO_GHFAST 覆盖）
#    3) 本机 HTTP 代理（HAO_HTTP_PROXY / http_proxy / proxy.local.ps1）
# ============================================================

$script:HaoGhFastDefault = 'https://ghfast.top'

function Import-HaoProxyLocal {
    param([string]$Root)
    if (-not $Root) { $Root = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path }
    $local = Join-Path $Root 'script\win\proxy.local.ps1'
    if (Test-Path -LiteralPath $local) {
        . $local
        Write-Host '[代理] 已加载 script\win\proxy.local.ps1' -ForegroundColor DarkGray
    }
}

function Get-HaoProxyUrl {
    foreach ($name in @('HAO_HTTP_PROXY', 'HTTPS_PROXY', 'https_proxy', 'HTTP_PROXY', 'http_proxy')) {
        $v = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ([string]::IsNullOrWhiteSpace($v)) {
            $v = [Environment]::GetEnvironmentVariable($name, 'User')
        }
        if (-not [string]::IsNullOrWhiteSpace($v)) { return $v.Trim() }
    }
    return $null
}

function Get-HaoGhFastBase {
    $b = $env:HAO_GHFAST
    if ([string]::IsNullOrWhiteSpace($b)) { $b = $script:HaoGhFastDefault }
    return $b.TrimEnd('/')
}

function Test-HaoIsGitHubUrl {
    param([string]$Url)
    if ([string]::IsNullOrWhiteSpace($Url)) { return $false }
    return [bool]($Url -match '(?i)^https?://(github\.com|raw\.githubusercontent\.com|gist\.githubusercontent\.com|gist\.github\.com)(/|$)')
}

function ConvertTo-HaoGhFastUrl {
    param([Parameter(Mandatory = $true)][string]$Url)
    if ($Url -match '(?i)^https?://ghfast\.top/') { return $Url }
    if ($env:HAO_GHFAST -and ($Url -match [regex]::Escape($env:HAO_GHFAST.TrimEnd('/')))) { return $Url }
    return "$(Get-HaoGhFastBase)/$Url"
}

function Enable-HaoProxySession {
    param([string]$ProxyUrl)
    if (-not $ProxyUrl) { $ProxyUrl = Get-HaoProxyUrl }
    if (-not $ProxyUrl) {
        Write-Host '[代理] 未配置 HTTP 代理。GitHub 会先试 ghfast，仍失败再考虑 proxy.local.ps1 / HAO_HTTP_PROXY' -ForegroundColor Yellow
        return $false
    }
    $env:http_proxy  = $ProxyUrl
    $env:https_proxy = $ProxyUrl
    $env:HTTP_PROXY  = $ProxyUrl
    $env:HTTPS_PROXY = $ProxyUrl
    $env:HAO_HTTP_PROXY = $ProxyUrl
    Write-Host "[代理] 会话已启用 HTTP 代理 $ProxyUrl" -ForegroundColor Yellow
    return $true
}

function Save-HaoProxyEnv {
    # 注意：PS 哈希键大小写不敏感，勿同时写 http_proxy 与 HTTP_PROXY
    return @{
        http_proxy     = $env:http_proxy
        https_proxy    = $env:https_proxy
        HAO_HTTP_PROXY = $env:HAO_HTTP_PROXY
        all_proxy      = $env:all_proxy
    }
}

function Restore-HaoProxyEnv {
    param($Saved)
    foreach ($k in @('http_proxy', 'https_proxy', 'HAO_HTTP_PROXY', 'all_proxy')) {
        $v = $Saved[$k]
        if ([string]::IsNullOrWhiteSpace($v)) {
            Remove-Item "env:$k" -ErrorAction Ignore
        } else {
            Set-Item -Path "env:$k" -Value $v
        }
    }
}

function Clear-HaoProxyEnv {
    Remove-Item env:http_proxy, env:https_proxy, env:all_proxy -ErrorAction Ignore
}

function Test-HaoNetworkOnce {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [string]$ProxyUrl,
        [int]$TimeoutSec = 12
    )
    $saved = Save-HaoProxyEnv
    try {
        if ($ProxyUrl) {
            $env:http_proxy = $ProxyUrl
            $env:https_proxy = $ProxyUrl
        } else {
            Clear-HaoProxyEnv
        }
        try {
            $req = [System.Net.HttpWebRequest]::Create($Url)
            $req.Method = 'HEAD'
            $req.Timeout = $TimeoutSec * 1000
            $req.AllowAutoRedirect = $true
            $req.UserAgent = 'HaoLang-setup'
            if ($ProxyUrl) {
                $req.Proxy = New-Object System.Net.WebProxy($ProxyUrl)
            } else {
                $req.Proxy = [System.Net.GlobalProxySelection]::GetEmptyWebProxy()
            }
            $resp = $req.GetResponse()
            $resp.Close()
            return $true
        } catch {
            try {
                $params = @{
                    Uri             = $Url
                    Method          = 'Head'
                    TimeoutSec      = $TimeoutSec
                    UseBasicParsing = $true
                    ErrorAction     = 'Stop'
                }
                if ($ProxyUrl) {
                    $params.Proxy = $ProxyUrl
                }
                Invoke-WebRequest @params | Out-Null
                return $true
            } catch {
                return $false
            }
        }
    } finally {
        Restore-HaoProxyEnv $saved
    }
}

function Test-HaoNetwork {
    param(
        [string]$Url = 'https://github.com',
        [int]$TimeoutSec = 12
    )
    # 非 GitHub：直连 → HTTP 代理
    if (-not (Test-HaoIsGitHubUrl $Url)) {
        if (Test-HaoNetworkOnce -Url $Url -ProxyUrl $null -TimeoutSec $TimeoutSec) { return $true }
        $p = Get-HaoProxyUrl
        if ($p -and (Test-HaoNetworkOnce -Url $Url -ProxyUrl $p -TimeoutSec $TimeoutSec)) { return $true }
        return $false
    }

    # GitHub：直连 → ghfast → HTTP 代理
    if (Test-HaoNetworkOnce -Url $Url -ProxyUrl $null -TimeoutSec $TimeoutSec) {
        Write-Host '[网络] GitHub 直连 OK' -ForegroundColor DarkGray
        return $true
    }
    $fast = ConvertTo-HaoGhFastUrl $Url
    if (Test-HaoNetworkOnce -Url $fast -ProxyUrl $null -TimeoutSec $TimeoutSec) {
        Write-Host "[网络] GitHub 直连失败，ghfast 可用 ($fast)" -ForegroundColor Yellow
        return $true
    }
    $p = Get-HaoProxyUrl
    if ($p -and (Test-HaoNetworkOnce -Url $Url -ProxyUrl $p -TimeoutSec $TimeoutSec)) {
        Write-Host "[网络] ghfast 不可用，HTTP 代理可用 ($p)" -ForegroundColor Yellow
        return $true
    }
    return $false
}

function Invoke-HaoDownloadOnce {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [string]$ProxyUrl
    )
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $name = Split-Path $OutFile -Leaf
    $destDir = if ($dir) { $dir } else { '.' }
    if (Test-Path -LiteralPath $OutFile) { Remove-Item -LiteralPath $OutFile -Force -ErrorAction Ignore }

    $saved = Save-HaoProxyEnv
    try {
        if ($ProxyUrl) {
            $env:http_proxy = $ProxyUrl
            $env:https_proxy = $ProxyUrl
            $env:HTTP_PROXY = $ProxyUrl
            $env:HTTPS_PROXY = $ProxyUrl
        } else {
            Clear-HaoProxyEnv
        }

        $aria = Get-Command aria2c -ErrorAction SilentlyContinue
        if ($aria) {
            $args = @(
                '-x4', '-s4', '-c',
                '--file-allocation=none',
                '--console-log-level=warn',
                '--summary-interval=0',
                '-d', $destDir,
                '-o', $name,
                $Url
            )
            if ($ProxyUrl) {
                $args = @("--all-proxy=$ProxyUrl") + $args
            } else {
                # 避免沿用环境代理
                $args = @('--all-proxy=') + $args
            }
            & aria2c @args
            if ($LASTEXITCODE -ne 0) { throw "aria2c 退出码 $LASTEXITCODE" }
        } else {
            if ($ProxyUrl) {
                Invoke-WebRequest -Uri $Url -OutFile $OutFile -Proxy $ProxyUrl
            } else {
                Invoke-WebRequest -Uri $Url -OutFile $OutFile
            }
        }
        if (-not (Test-Path -LiteralPath $OutFile)) { throw '文件未生成' }
        if ((Get-Item -LiteralPath $OutFile).Length -le 0) { throw '文件大小为 0' }
    } finally {
        Restore-HaoProxyEnv $saved
    }
}

function Invoke-HaoDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [string]$ProxyUrl
    )
    # 显式传入 ProxyUrl 时只走这一路（兼容旧调用）
    if ($ProxyUrl) {
        Invoke-HaoDownloadOnce -Url $Url -OutFile $OutFile -ProxyUrl $ProxyUrl
        return
    }

    $attempts = New-Object System.Collections.Generic.List[object]
    if (Test-HaoIsGitHubUrl $Url) {
        [void]$attempts.Add(@{ Label = '直连'; Url = $Url; Proxy = $null })
        [void]$attempts.Add(@{ Label = 'ghfast'; Url = (ConvertTo-HaoGhFastUrl $Url); Proxy = $null })
        $p = Get-HaoProxyUrl
        if ($p) { [void]$attempts.Add(@{ Label = 'http_proxy'; Url = $Url; Proxy = $p }) }
    } else {
        [void]$attempts.Add(@{ Label = '直连'; Url = $Url; Proxy = $null })
        $p = Get-HaoProxyUrl
        if ($p) { [void]$attempts.Add(@{ Label = 'http_proxy'; Url = $Url; Proxy = $p }) }
    }

    $errors = @()
    foreach ($a in $attempts) {
        Write-Host "[下载] 尝试 $($a.Label): $($a.Url)" -ForegroundColor Cyan
        try {
            Invoke-HaoDownloadOnce -Url $a.Url -OutFile $OutFile -ProxyUrl $a.Proxy
            Write-Host "[下载] $($a.Label) 成功" -ForegroundColor Green
            return
        } catch {
            $msg = $_.Exception.Message
            Write-Host "[下载] $($a.Label) 失败: $msg" -ForegroundColor DarkYellow
            $errors += "$($a.Label): $msg"
            if (Test-Path -LiteralPath $OutFile) { Remove-Item -LiteralPath $OutFile -Force -ErrorAction Ignore }
        }
    }

    $hint = ''
    if (Test-HaoIsGitHubUrl $Url) {
        $hint = "；可配置 script\win\proxy.local.ps1 或 HAO_HTTP_PROXY；也可设 HAO_GHFAST 换镜像站"
    }
    throw ("下载失败: $Url`n  " + ($errors -join "`n  ") + $hint)
}

function Ensure-HaoScoop {
    param([string]$Root)
    $scoopCmd = Get-Command scoop -ErrorAction SilentlyContinue
    if ($scoopCmd) { return $true }

    Write-Host '[scoop] 未找到，尝试安装...' -ForegroundColor Cyan
    # get.scoop.sh 本身非 GitHub；安装过程会拉 GitHub，先确认 GitHub 三路可达
    if (-not (Test-HaoNetwork -Url 'https://github.com')) {
        Write-Host '[网络] GitHub 直连 / ghfast / HTTP 代理均不可用。' -ForegroundColor Red
        Write-Host '  1) 确认能打开 https://ghfast.top' -ForegroundColor Yellow
        Write-Host '  2) 或复制 script\win\proxy.local.ps1.example -> proxy.local.ps1 填 HTTP 代理' -ForegroundColor Yellow
        Write-Host '  3) 或设置 $env:HAO_HTTP_PROXY = "http://127.0.0.1:端口"' -ForegroundColor Yellow
        return $false
    }

    $saved = Save-HaoProxyEnv
    try {
        # scoop 安装脚本：优先无代理；失败再用 HTTP 代理
        Clear-HaoProxyEnv
        try {
            Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force -ErrorAction SilentlyContinue
            Invoke-RestMethod -Uri 'https://get.scoop.sh' | Invoke-Expression
        } catch {
            $p = Get-HaoProxyUrl
            if (-not $p) { throw }
            Write-Host '[scoop] 直连安装失败，改用 HTTP 代理…' -ForegroundColor Yellow
            $env:http_proxy = $p
            $env:https_proxy = $p
            Invoke-RestMethod -Uri 'https://get.scoop.sh' | Invoke-Expression
        }
    } catch {
        Write-Host "[scoop] 安装失败: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host '请手动安装 scoop: https://scoop.sh' -ForegroundColor Yellow
        Restore-HaoProxyEnv $saved
        return $false
    }
    Restore-HaoProxyEnv $saved

    $scoopCmd = Get-Command scoop -ErrorAction SilentlyContinue
    if (-not $scoopCmd) {
        $shim = Join-Path $env:USERPROFILE 'scoop\shims'
        if (Test-Path $shim) { $env:PATH = "$shim;$env:PATH" }
        $scoopCmd = Get-Command scoop -ErrorAction SilentlyContinue
    }
    if (-not $scoopCmd) {
        Write-Host '[scoop] 安装后仍找不到 scoop 命令，请新开终端重试' -ForegroundColor Red
        return $false
    }
    Write-Host '[scoop] 安装完成' -ForegroundColor Green
    return $true
}

function Install-HaoScoopPackage {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$CheckCommand
    )
    if ($CheckCommand) {
        if (Get-Command $CheckCommand -ErrorAction SilentlyContinue) { return $true }
    }
    Write-Host "[scoop] 安装 $Name ..." -ForegroundColor Cyan
    & scoop install $Name
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[scoop] 安装 $Name 失败" -ForegroundColor Red
        return $false
    }
    return $true
}