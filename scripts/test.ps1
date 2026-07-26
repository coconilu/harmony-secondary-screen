param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'static-check.ps1')
if ($LASTEXITCODE -ne 0) { throw "Static checks failed: $LASTEXITCODE" }

Push-Location (Join-Path (Split-Path -Parent $PSScriptRoot) 'extension')
try {
  & npm test
  if ($LASTEXITCODE -ne 0) { throw "Edge extension tests failed: $LASTEXITCODE" }
  & npm audit --audit-level=high
  if ($LASTEXITCODE -ne 0) { throw "Edge extension dependency audit failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}

& (Join-Path $PSScriptRoot 'test-receiver-protocol.ps1')
if ($LASTEXITCODE -ne 0) { throw "Receiver protocol tests failed: $LASTEXITCODE" }

& (Join-Path $PSScriptRoot 'build-receiver.ps1') -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw "Receiver build failed: $LASTEXITCODE" }

Write-Host 'Direct Edge-to-Receiver tests and build passed. Real Edge and tablet acceptance remain separate.'
