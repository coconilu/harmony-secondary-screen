param(
  [string]$DevEcoRoot = $env:HSS_DEVECO_ROOT,
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$receiverRoot = Join-Path $projectRoot 'receiver'
if ([string]::IsNullOrWhiteSpace($DevEcoRoot)) {
  $DevEcoRoot = Join-Path $env:ProgramFiles 'Huawei\DevEco Studio'
}
$DevEcoRoot = [System.IO.Path]::GetFullPath($DevEcoRoot)

$node = Join-Path $DevEcoRoot 'tools\node\node.exe'
$npm = Join-Path $DevEcoRoot 'tools\node\npm.cmd'
$ohpm = Join-Path $DevEcoRoot 'tools\ohpm\bin\ohpm.bat'
$bundledPluginPackage = Join-Path $DevEcoRoot 'tools\hvigor\hvigor-ohos-plugin\package.json'
$sdkRoot = Join-Path $DevEcoRoot 'sdk'
$jbrRoot = Join-Path $DevEcoRoot 'jbr'

$requiredPaths = @($node, $npm, $ohpm, $bundledPluginPackage, $sdkRoot, $jbrRoot)
$missingPaths = @($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingPaths.Count -gt 0) {
  $checked = $requiredPaths -join "`n  - "
  $missing = $missingPaths -join "`n  - "
  throw @"
DevEco Studio toolchain is incomplete.
DevEco root: $DevEcoRoot
Missing:
  - $missing
Checked:
  - $checked
Install a supported DevEco Studio version or set HSS_DEVECO_ROOT to its installation root.
"@
}

try {
  $bundledPlugin = Get-Content -LiteralPath $bundledPluginPackage -Raw -Encoding UTF8 |
    ConvertFrom-Json
} catch {
  throw "Cannot read the bundled Hvigor plugin metadata: $bundledPluginPackage`n$($_.Exception.Message)"
}
$hvigorVersion = [string]$bundledPlugin.version
if ($hvigorVersion -notmatch '^\d+\.\d+\.\d+$') {
  throw "Unsupported bundled Hvigor plugin version '$hvigorVersion': $bundledPluginPackage"
}

$buildToolsRoot = Join-Path $projectRoot 'out\harmony-build-tools'
$toolchainRoot = Join-Path $buildToolsRoot "hvigor-$hvigorVersion"
$toolchainModules = Join-Path $toolchainRoot 'node_modules'
$hvigor = Join-Path $toolchainModules '@ohos\hvigor\bin\hvigor.js'
$localEnginePackage = Join-Path $toolchainModules '@ohos\hvigor\package.json'
$localPluginPackage = Join-Path $toolchainModules '@ohos\hvigor-ohos-plugin\package.json'
$hvigorUserHome = Join-Path $buildToolsRoot 'hvigor-user-home'
$npmCache = Join-Path $buildToolsRoot 'npm-cache'
$ohpmCache = Join-Path $buildToolsRoot 'ohpm-cache'

function Get-PackageVersion {
  param([string]$PackageFile)

  if (-not (Test-Path -LiteralPath $PackageFile)) {
    return ''
  }
  try {
    return [string]((Get-Content -LiteralPath $PackageFile -Raw -Encoding UTF8 |
      ConvertFrom-Json).version)
  } catch {
    return ''
  }
}

$engineVersion = Get-PackageVersion $localEnginePackage
$pluginVersion = Get-PackageVersion $localPluginPackage
$needsToolchain = -not (Test-Path -LiteralPath $hvigor) -or
  $engineVersion -ne $hvigorVersion -or
  $pluginVersion -ne $hvigorVersion

if ($needsToolchain) {
  New-Item -ItemType Directory -Path $toolchainRoot -Force | Out-Null
  Write-Host "Installing isolated Hvigor $hvigorVersion build tools..."
  & $npm install --prefix $toolchainRoot `
    "@ohos/hvigor@$hvigorVersion" `
    "@ohos/hvigor-ohos-plugin@$hvigorVersion" `
    --registry 'https://repo.harmonyos.com/npm/' `
    --cache $npmCache `
    --no-audit `
    --no-fund `
    --package-lock=false `
    --legacy-peer-deps
  if ($LASTEXITCODE -ne 0) {
    throw "Isolated Hvigor toolchain install failed: $LASTEXITCODE"
  }
}

$engineVersion = Get-PackageVersion $localEnginePackage
$pluginVersion = Get-PackageVersion $localPluginPackage
if (-not (Test-Path -LiteralPath $hvigor) -or
    $engineVersion -ne $hvigorVersion -or
    $pluginVersion -ne $hvigorVersion) {
  throw @"
Isolated Hvigor toolchain validation failed.
Expected: $hvigorVersion
Engine: $engineVersion
Plugin: $pluginVersion
Toolchain: $toolchainRoot
"@
}

$environmentNames = @(
  'DEVECO_SDK_HOME',
  'JAVA_HOME',
  'NODE_HOME',
  'NODE_PATH',
  'HVIGOR_USER_HOME',
  'Path'
)
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
  $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
  $env:DEVECO_SDK_HOME = $sdkRoot
  $env:JAVA_HOME = $jbrRoot
  $env:NODE_HOME = Split-Path -Parent $node
  $env:NODE_PATH = $toolchainModules
  $env:HVIGOR_USER_HOME = $hvigorUserHome
  $env:Path = "$(Join-Path $jbrRoot 'bin');$env:NODE_HOME;$env:Path"

  Push-Location $receiverRoot
  try {
    & $ohpm install --cache $ohpmCache
    if ($LASTEXITCODE -ne 0) { throw "ohpm install failed: $LASTEXITCODE" }

    $buildMode = $Configuration.ToLowerInvariant()
    & $node $hvigor assembleApp -p "buildMode=$buildMode" --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "assembleApp failed: $LASTEXITCODE" }
  } finally {
    Pop-Location
  }

  $expectedArtifact = Join-Path $receiverRoot `
    'entry\build\default\outputs\default\entry-default-unsigned.hap'
  if (-not (Test-Path -LiteralPath $expectedArtifact -PathType Leaf)) {
    throw "Expected unsigned Receiver artifact was not produced: $expectedArtifact"
  }
  Write-Host "Receiver $Configuration unsigned HAP: $expectedArtifact"
} finally {
  foreach ($name in $environmentNames) {
    [Environment]::SetEnvironmentVariable(
      $name,
      $previousEnvironment[$name],
      'Process'
    )
  }
}
