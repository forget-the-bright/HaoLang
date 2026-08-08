# ============================================================
#  HaoLang 环境准备入口（转发到 Windows 脚本）
#  用法:
#    powershell -ExecutionPolicy Bypass -File script\setup_env.ps1
#    powershell -ExecutionPolicy Bypass -File script\setup_env.ps1 -SkipLinux
#  实现：script\win\setup_env.ps1
# ============================================================
param(
    [switch]$SkipLinux,
    [switch]$SkipMsvc,
    [switch]$ForceLlvm
)
& "$PSScriptRoot\win\setup_env.ps1" @PSBoundParameters
exit $LASTEXITCODE