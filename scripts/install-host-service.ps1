param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [switch]$Start,
  [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$serviceName = 'HarmonySecondaryScreenHost'
$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw '请在管理员 PowerShell 中运行此脚本。'
}

$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($Uninstall) {
  if ($null -eq $existing) {
    Write-Host '服务未安装。'
    exit 0
  }
  if ($existing.Status -ne 'Stopped') { Stop-Service -Name $serviceName -Force }
  & sc.exe delete $serviceName
  if ($LASTEXITCODE -ne 0) { throw "删除服务失败: $LASTEXITCODE" }
  exit 0
}

if ($null -ne $existing) {
  throw "服务 $serviceName 已存在；如需重装，请先使用 -Uninstall。"
}
$hostExe = Join-Path (Split-Path -Parent $PSScriptRoot) "out\host\$Configuration\hss_host.exe"
if (-not (Test-Path -LiteralPath $hostExe)) {
  throw "找不到 $hostExe；请先运行 build-host.ps1。"
}
$binaryPath = '"{0}" --service' -f (Resolve-Path -LiteralPath $hostExe).Path
New-Service -Name $serviceName -BinaryPathName $binaryPath -DisplayName 'Harmony Secondary Screen Host' `
  -Description 'Private-LAN control and video service for Harmony Secondary Screen.' `
  -StartupType Manual | Out-Null
Write-Host "已安装服务 $serviceName（手动启动）。"
if ($Start) { Start-Service -Name $serviceName }
