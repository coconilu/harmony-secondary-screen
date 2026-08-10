[CmdletBinding(SupportsShouldProcess)]
param(
  [ValidatePattern('^[a-p]{32}$')]
  [string]$ExtensionId,

  [string]$RelayExecutable,

  [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$hostName = 'com.coconilu.harmony_web_companion'
$registryPath = "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\$hostName"
$installRoot = Join-Path $env:LOCALAPPDATA 'HarmonyWebCompanion\NativeHost'
$installedExecutable = Join-Path $installRoot 'harmony_web_companion_relay.exe'
$manifestPath = Join-Path $installRoot "$hostName.json"

if ($Uninstall) {
  if ($PSCmdlet.ShouldProcess($registryPath, 'Remove Edge Native Messaging registration')) {
    Remove-Item -LiteralPath $registryPath -Recurse -Force -ErrorAction SilentlyContinue
  }

  $localAppDataRoot = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + '\'
  $resolvedInstallRoot = [IO.Path]::GetFullPath($installRoot)
  if (-not $resolvedInstallRoot.StartsWith(
      $localAppDataRoot,
      [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove a path outside LOCALAPPDATA: $resolvedInstallRoot"
  }
  if ($PSCmdlet.ShouldProcess($resolvedInstallRoot, 'Remove Relay Native Host files')) {
    Remove-Item -LiteralPath $resolvedInstallRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
  Write-Host 'Relay Native Host registration removed for the current user.'
  exit 0
}

if (-not $ExtensionId) {
  throw 'ExtensionId is required. Copy it from edge://extensions/.'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $RelayExecutable) {
  $RelayExecutable = Join-Path $projectRoot 'out\relay\Release\harmony_web_companion_relay.exe'
}
$RelayExecutable = [IO.Path]::GetFullPath($RelayExecutable)
if (-not (Test-Path -LiteralPath $RelayExecutable -PathType Leaf)) {
  throw "Relay executable not found: $RelayExecutable. Run scripts\build-relay.ps1 first."
}

if ($PSCmdlet.ShouldProcess($installRoot, 'Install Relay Native Host for the current user')) {
  New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
  Copy-Item -LiteralPath $RelayExecutable -Destination $installedExecutable -Force

  $manifest = [ordered]@{
    name = $hostName
    description = 'TabReach loopback Relay'
    path = $installedExecutable
    type = 'stdio'
    allowed_origins = @("chrome-extension://$ExtensionId/")
  }
  $manifestJson = $manifest | ConvertTo-Json -Depth 4
  $utf8NoBom = [Text.UTF8Encoding]::new($false)
  [IO.File]::WriteAllText($manifestPath, $manifestJson, $utf8NoBom)

  New-Item -Path $registryPath -Force | Out-Null
  Set-Item -LiteralPath $registryPath -Value $manifestPath
}

if ($WhatIfPreference) {
  Write-Host 'WhatIf completed; no Native Messaging files or registry values were changed.'
  exit 0
}

$registeredManifest = (Get-Item -LiteralPath $registryPath).GetValue('')
if ($registeredManifest -ne $manifestPath) {
  throw 'Native Messaging registry read-back did not match the installed manifest.'
}
$readBack = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
  ConvertFrom-Json
if ($readBack.name -ne $hostName -or
    $readBack.path -ne $installedExecutable -or
    @($readBack.allowed_origins) -notcontains "chrome-extension://$ExtensionId/") {
  throw 'Native Messaging manifest read-back validation failed.'
}

Write-Host "Relay Native Host registered for Edge extension $ExtensionId."
Write-Host "Manifest: $manifestPath"
