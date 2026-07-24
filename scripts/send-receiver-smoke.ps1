param(
  [Parameter(Mandatory)]
  [string]$ReceiverAddress,

  [Parameter(Mandatory)]
  [ValidatePattern('^\d{6}$')]
  [string]$PairingCode
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'out\receiver-smoke'
$h264Path = Join-Path $outputDirectory 'single-keyframe.h264'
$senderPath = Join-Path $projectRoot 'relay\tools\receiver_smoke_sender.mjs'

$ffmpeg = Get-Command ffmpeg -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

& $ffmpeg.Source -hide_banner -loglevel error -y `
  -f lavfi -i 'color=c=0x0B111A:size=1280x720:rate=30,drawbox=x=80:y=80:w=1120:h=560:color=0x14B8A6:t=fill,drawbox=x=120:y=120:w=1040:h=480:color=0x0F172A:t=fill,drawbox=x=160:y=160:w=320:h=320:color=0x22C55E:t=fill,drawbox=x=520:y=160:w=600:h=140:color=white:t=fill,drawbox=x=520:y=340:w=600:h=140:color=0x38BDF8:t=fill' `
  -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency `
  -profile:v baseline -pix_fmt yuv420p `
  -x264-params 'keyint=1:min-keyint=1:scenecut=0:repeat-headers=1' `
  -f h264 $h264Path
if ($LASTEXITCODE -ne 0) {
  throw "ffmpeg failed to create the H.264 smoke frame: $LASTEXITCODE"
}

& node $senderPath $ReceiverAddress $PairingCode $h264Path
if ($LASTEXITCODE -ne 0) {
  throw "Receiver smoke sender failed: $LASTEXITCODE"
}
