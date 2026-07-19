$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$receiverRoot = Join-Path $projectRoot 'receiver'
$devecoRoot = 'C:\Program Files\Huawei\DevEco Studio'
$node = Join-Path $devecoRoot 'tools\node\node.exe'
$hvigor = Join-Path $devecoRoot 'tools\hvigor\hvigor\bin\hvigor.js'
$hvigorEngine = Join-Path $devecoRoot 'tools\hvigor\hvigor'
$ohpm = Join-Path $devecoRoot 'tools\ohpm\bin\ohpm.bat'
$hvigorPlugin = Join-Path $devecoRoot 'tools\hvigor\hvigor-ohos-plugin'
$sdkRoot = Join-Path $devecoRoot 'sdk'
$jbrRoot = Join-Path $devecoRoot 'jbr'

foreach ($requiredPath in @($node, $hvigor, $hvigorEngine, $ohpm, $hvigorPlugin, $sdkRoot, $jbrRoot)) {
  if (-not (Test-Path -LiteralPath $requiredPath)) {
    throw "Missing DevEco dependency: $requiredPath"
  }
}
$env:DEVECO_SDK_HOME = $sdkRoot
$env:JAVA_HOME = $jbrRoot
$env:Path = "$(Join-Path $jbrRoot 'bin');$env:Path"

# DevEco normally materializes this scoped package while opening a project.
# The CLI build creates an ignored junction to the bundled plugin instead.
$scopedNodeModules = Join-Path $receiverRoot 'node_modules\@ohos'
$pluginLink = Join-Path $scopedNodeModules 'hvigor-ohos-plugin'
$engineLink = Join-Path $scopedNodeModules 'hvigor'
if (-not (Test-Path -LiteralPath $pluginLink)) {
  New-Item -ItemType Directory -Path $scopedNodeModules -Force | Out-Null
  New-Item -ItemType Junction -Path $pluginLink -Target $hvigorPlugin | Out-Null
}
if (-not (Test-Path -LiteralPath $engineLink)) {
  New-Item -ItemType Junction -Path $engineLink -Target $hvigorEngine | Out-Null
}
$env:NODE_PATH = Join-Path $receiverRoot 'node_modules'

Push-Location $receiverRoot
try {
  & $ohpm install
  if ($LASTEXITCODE -ne 0) { throw "ohpm install failed: $LASTEXITCODE" }

  & $node $hvigor assembleApp --no-daemon
  if ($LASTEXITCODE -ne 0) { throw "assembleApp failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}
