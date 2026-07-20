param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [switch]$Start,
  [switch]$Uninstall,
  [switch]$ValidateFirewallLookup
)

$ErrorActionPreference = 'Stop'
$serviceName = 'HarmonySecondaryScreenHost'
$firewallRuleName = 'HarmonySecondaryScreenHost-Control'

function Get-FirewallRulesByExactName([string]$Name) {
  Get-NetFirewallRule -ErrorAction Stop |
    Where-Object { $_.Name -eq $Name }
}

function Remove-HostFirewallRule {
  $rules = @(Get-FirewallRulesByExactName $firewallRuleName)
  if ($rules.Count -gt 0) {
    $rules | Remove-NetFirewallRule -ErrorAction Stop
  }
}

if ($ValidateFirewallLookup) {
  $probeName = 'HarmonySecondaryScreenHost-Control-Definitely-Missing-Validation-Probe'
  $probeRules = @(Get-FirewallRulesByExactName $probeName)
  if ($probeRules.Count -ne 0) {
    throw "Firewall lookup validation probe unexpectedly exists: $probeName"
  }
  Write-Host 'Firewall missing-rule lookup contract passed.'
  return
}

$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated PowerShell window.'
}

$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($Uninstall) {
  $cleanupErrors = @()
  if ($null -ne $existing) {
    if ($existing.Status -ne 'Stopped') {
      try {
        Stop-Service -Name $serviceName -Force -ErrorAction Stop
      } catch {
        $cleanupErrors += "Failed to stop the service: $($_.Exception.Message)"
      }
    }
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) {
      $cleanupErrors += "Failed to delete the service: $LASTEXITCODE"
    }
  }
  try {
    Remove-HostFirewallRule
  } catch {
    $cleanupErrors += "Failed to delete the firewall rule: $($_.Exception.Message)"
  }
  if ($cleanupErrors.Count -gt 0) {
    throw ($cleanupErrors -join [Environment]::NewLine)
  }
  Write-Host 'Removed the Host service and firewall rule.'
  exit 0
}

if ($null -ne $existing) {
  throw "Service $serviceName already exists; run with -Uninstall before reinstalling."
}
$hostExe = Join-Path (Split-Path -Parent $PSScriptRoot) "out\host\$Configuration\hss_host.exe"
if (-not (Test-Path -LiteralPath $hostExe)) {
  throw "Cannot find $hostExe; run build-host.ps1 first."
}

# This confirmation never runs inside Session 0. The selected Network List Manager profile ID
# becomes an app-owned service argument; Windows network category and VPN settings are untouched.
$trustedNetworkId = (& $hostExe --prepare-network | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($trustedNetworkId)) {
  throw 'The current physical Wi-Fi was not approved; the Host service was not installed.'
}
$trustedNetworkId = $trustedNetworkId.Trim()
if ($trustedNetworkId -notmatch '^\{[0-9A-Fa-f-]{36}\}$') {
  throw "The Host returned an invalid Wi-Fi profile ID: $trustedNetworkId"
}

$resolvedHostExe = (Resolve-Path -LiteralPath $hostExe).Path
$binaryPath = '"{0}" --service "{1}"' -f $resolvedHostExe, $trustedNetworkId
$serviceCreated = $false
try {
  New-Service -Name $serviceName -BinaryPathName $binaryPath -DisplayName 'Harmony Secondary Screen Host' `
    -Description 'Trusted physical Wi-Fi control and video service for Harmony Secondary Screen.' `
    -StartupType Manual | Out-Null
  $serviceCreated = $true
  Remove-HostFirewallRule
  New-NetFirewallRule -Name $firewallRuleName -DisplayName 'Harmony Secondary Screen control' `
    -Direction Inbound -Action Allow -Enabled True -Profile Any -InterfaceType Wireless `
    -Program $resolvedHostExe -Protocol TCP -LocalPort 47100 -RemoteAddress LocalSubnet | Out-Null
  if ($Start) { Start-Service -Name $serviceName }
} catch {
  $installFailure = $_
  $rollbackErrors = @()
  if ($serviceCreated) {
    try {
      Stop-Service -Name $serviceName -Force -ErrorAction Stop
    } catch {
      $rollbackErrors += "Failed to stop the service during rollback: $($_.Exception.Message)"
    }
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) {
      $rollbackErrors += "Failed to delete the service during rollback: $LASTEXITCODE"
    }
  }
  try {
    Remove-HostFirewallRule
  } catch {
    $rollbackErrors += "Failed to delete the firewall rule during rollback: $($_.Exception.Message)"
  }
  if ($rollbackErrors.Count -gt 0) {
    throw "Host installation failed: $($installFailure.Exception.Message)$([Environment]::NewLine)$($rollbackErrors -join [Environment]::NewLine)"
  }
  throw $installFailure
}
Write-Host "Installed service $serviceName with Manual startup."
