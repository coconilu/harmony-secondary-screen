$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
  $forbidden = & rg -n -i '(@ohos\.web\.webview|\bWebView\b|<html|android\.intent|androidx\.|\.apk\b)' receiver 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a forbidden WebView/HTML5/Android dependency:`n$forbidden"
  }
  if ($LASTEXITCODE -ne 1) { throw "Forbidden dependency scan failed: $LASTEXITCODE" }

  $invalidIddType = & rg -n --fixed-strings 'IDDCX_MONITOR_TYPE_' host\driver 2>$null
  if ($LASTEXITCODE -eq 0) { throw "Invalid IddCx monitor type found:`n$invalidIddType" }
  if ($LASTEXITCODE -ne 1) { throw "IddCx invalid-type scan failed: $LASTEXITCODE" }

  $sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
  $wingdi = Get-ChildItem $sdkInclude -Recurse -Filter wingdi.h -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
  if ($null -eq $wingdi) { throw 'Windows SDK wingdi.h is missing.' }
  & rg -q --fixed-strings 'DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER' $wingdi.FullName
  if ($LASTEXITCODE -ne 0) { throw 'Installed Windows SDK lacks DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER.' }

  $requiredPatterns = @(
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'IddCxAdapterInitAsync' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'IddCxSwapChainReleaseAndAcquireBuffer' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'info.MonitorDescription.Size = sizeof(info.MonitorDescription)' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'OpenEventW(SYNCHRONIZE' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'ComMtaApartment' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'D3D11_VIDEO_PROCESSOR' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'MFTEnumEx' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'METransformNeedInput' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'METransformHaveOutput' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'FallbackToSoftware' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'AsyncEventLoop' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'pending_inputs_.size() >= 3' },
    @{ Path = 'host\app\local_security.h'; Pattern = 'GW;;;LS' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'PIPE_REJECT_REMOTE_CLIENTS' },
    @{ Path = 'host\app\network_gate.cpp'; Pattern = 'NLM_NETWORK_CATEGORY_PRIVATE' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'IsPrivateIpv4(localAddress' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.Revoke' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.CanSend' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.RunIfAllowed' },
    @{ Path = 'host\app\main.cpp'; Pattern = 'StartServiceCtrlDispatcherW' },
    @{ Path = 'host\app\pointer_relay.cpp'; Pattern = 'HarmonySecondaryScreen.Input' },
    @{ Path = 'host\app\input_agent.cpp'; Pattern = 'PointerInjector' },
    @{ Path = 'host\app\pointer_injector.cpp'; Pattern = 'QueryDisplayConfig' },
    @{ Path = 'host\app\pointer_injector.cpp'; Pattern = 'root#harmonysecondaryscreenidd' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_CreateByMime' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'O_NONBLOCK' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'connectDeadline' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_Flush' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_Start' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_lifecycle_mutex_' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_queue_mutex_' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'DecoderLifecycleState::kStopping' },
    @{ Path = 'receiver\entry\src\main\cpp\napi_init.cpp'; Pattern = 'OH_NativeXComponent_RegisterCallback' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'XComponentType.SURFACE' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'sessionShort' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'network_not_private' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'requireCodecConfig' }
  )
  foreach ($check in $requiredPatterns) {
    & rg -q --fixed-strings $check.Pattern $check.Path
    if ($LASTEXITCODE -ne 0) { throw "Required implementation marker missing: $($check.Pattern) in $($check.Path)" }
  }

  Write-Host 'Static architecture and forbidden-dependency checks passed.'
} finally {
  Pop-Location
}
