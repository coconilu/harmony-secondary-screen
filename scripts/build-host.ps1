param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [switch]$SkipDriver
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$hostRoot = Join-Path $projectRoot 'host'
$buildRoot = Join-Path $projectRoot 'out\host'

& cmake -S $hostRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw "Host CMake configure failed: $LASTEXITCODE" }

& cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Host application build failed: $LASTEXITCODE" }

& ctest --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Host tests failed: $LASTEXITCODE" }

if ($SkipDriver) {
  Write-Host 'Skipped IddCx driver build by explicit -SkipDriver.'
  exit 0
}

$wdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$iddHeader = Get-ChildItem (Join-Path $wdkRoot 'Include') -Recurse -Filter 'iddcx.h' -ErrorAction SilentlyContinue |
  Select-Object -First 1
if ($null -eq $iddHeader) {
  throw 'Windows Driver Kit IddCx headers are missing. Install the Windows 11 WDK and the Visual Studio Windows Driver Kit extension; the driver build was NOT run.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe is missing.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
  Select-Object -First 1
if (-not $msbuild) {
  $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
}
if (-not $msbuild) { throw 'MSBuild with the WDK toolset is missing.' }

$driverProject = Join-Path $hostRoot 'driver\HssIddDriver.vcxproj'
& $msbuild $driverProject /m /restore "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "IddCx driver build failed: $LASTEXITCODE" }
