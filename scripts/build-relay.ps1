param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$relayRoot = Join-Path $projectRoot 'relay'
$buildRoot = Join-Path $projectRoot 'out\relay'

& cmake -S $relayRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) {
  throw "Relay CMake configure failed: $LASTEXITCODE"
}

& cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
  throw "Relay build failed: $LASTEXITCODE"
}

& ctest --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
  throw "Relay tests failed: $LASTEXITCODE"
}

& node --test (Join-Path $relayRoot 'tests\receiver_smoke_sender_test.mjs')
if ($LASTEXITCODE -ne 0) {
  throw "Receiver smoke sender tests failed: $LASTEXITCODE"
}

$relayExecutable = Join-Path $buildRoot "$Configuration\harmony_web_companion_relay.exe"
& node (Join-Path $relayRoot 'tests\relay_integration_test.mjs') $relayExecutable
if ($LASTEXITCODE -ne 0) {
  throw "Relay integration test failed: $LASTEXITCODE"
}

Write-Host "Relay build and tests passed: $relayExecutable"
