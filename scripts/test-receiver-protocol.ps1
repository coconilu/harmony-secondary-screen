$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'out\receiver-protocol-tests'
$wireFixture = Join-Path $buildRoot 'extension-wire-message.bin'

node (Join-Path $projectRoot 'receiver\tests\write_extension_wire_fixture.mjs') $wireFixture
if ($LASTEXITCODE -ne 0) { throw "Extension wire fixture generation failed: $LASTEXITCODE" }

cmake -S (Join-Path $projectRoot 'receiver\tests') -B $buildRoot -A x64 `
  "-DHSS_EXTENSION_WIRE_FIXTURE=$wireFixture"
if ($LASTEXITCODE -ne 0) { throw "Receiver protocol configure failed: $LASTEXITCODE" }

cmake --build $buildRoot --config Release
if ($LASTEXITCODE -ne 0) { throw "Receiver protocol build failed: $LASTEXITCODE" }

ctest --test-dir $buildRoot -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Receiver protocol tests failed: $LASTEXITCODE" }

Write-Host 'Receiver direct WebSocket protocol tests passed.'
