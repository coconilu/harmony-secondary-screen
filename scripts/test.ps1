param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'static-check.ps1')
& (Join-Path $PSScriptRoot 'build-relay.ps1') -Configuration $Configuration
Push-Location (Join-Path (Split-Path -Parent $PSScriptRoot) 'extension')
try {
  & npm test
  if ($LASTEXITCODE -ne 0) {
    throw "Edge extension tests failed: $LASTEXITCODE"
  }
} finally {
  Pop-Location
}
& (Join-Path $PSScriptRoot 'build-host.ps1') -Configuration $Configuration -SkipDriver
& (Join-Path $PSScriptRoot 'build-receiver.ps1')

Write-Host 'Target builds/tests and retained legacy builds passed. Real Edge and tablet acceptance remain separate.'
