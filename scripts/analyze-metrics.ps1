param(
  [string]$HostFrames = '.\hss-host-frames.csv',
  [string]$ReceiverTelemetry = '.\hss-receiver-telemetry.csv'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $HostFrames)) { throw "Missing Host metrics: $HostFrames" }
if (-not (Test-Path -LiteralPath $ReceiverTelemetry)) { throw "Missing Receiver metrics: $ReceiverTelemetry" }

$frames = @(Import-Csv -LiteralPath $HostFrames)
$telemetry = @(Import-Csv -LiteralPath $ReceiverTelemetry)
if ($frames.Count -lt 2) { throw 'At least two Host frame records are required.' }
if ($telemetry.Count -lt 1) { throw 'At least one Receiver telemetry record is required.' }

$durationSeconds = ([double]$frames[-1].capture_us - [double]$frames[0].capture_us) / 1_000_000.0
if ($durationSeconds -le 0) { throw 'Host frame timestamps are not increasing.' }
$totalBytes = ($frames | Measure-Object -Property encoded_bytes -Sum).Sum
$averageMbps = ([double]$totalBytes * 8.0) / $durationSeconds / 1_000_000.0

$latencies = @($telemetry | ForEach-Object { [double]$_.end_to_end_us / 1000.0 } | Sort-Object)
function Get-NearestRank([double[]]$Values, [double]$Percentile) {
  $index = [Math]::Max(0, [Math]::Ceiling($Values.Count * $Percentile) - 1)
  return $Values[$index]
}

$last = $telemetry[-1]
$decoded = [double]$last.frames_decoded
$dropped = [double]$last.frames_dropped
$dropRate = if (($decoded + $dropped) -gt 0) { $dropped / ($decoded + $dropped) } else { 0 }

[pscustomobject]@{
  DurationMinutes = [Math]::Round($durationSeconds / 60.0, 3)
  EncodedFrames = $frames.Count
  AverageMbps = [Math]::Round($averageMbps, 3)
  SoftwareP50Ms = [Math]::Round((Get-NearestRank $latencies 0.50), 3)
  SoftwareP95Ms = [Math]::Round((Get-NearestRank $latencies 0.95), 3)
  ReceiverDecoded = [int64]$decoded
  ReceiverDropped = [int64]$dropped
  ReceiverDropRatePercent = [Math]::Round($dropRate * 100.0, 4)
  Warning = 'Software latency is diagnostic only; use the high-speed-camera method for acceptance.'
} | Format-List
