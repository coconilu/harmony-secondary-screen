param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'static-check.ps1')
& (Join-Path $PSScriptRoot 'build-host.ps1') -Configuration $Configuration -SkipDriver
& (Join-Path $PSScriptRoot 'build-receiver.ps1')

Write-Host 'All locally runnable checks passed. IddCx driver installation and real-device acceptance remain separate gates.'
