$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
  $forbidden = & rg -n -i '(@ohos\.web\.webview|\bWebView\b|<html|android\.intent|androidx\.|\.apk\b)' receiver 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a forbidden WebView/HTML5/Android dependency:`n$forbidden"
  }
  if ($LASTEXITCODE -ne 1) { throw "Forbidden dependency scan failed: $LASTEXITCODE" }

  $requiredPatterns = @(
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'IddCxAdapterInitAsync' },
    @{ Path = 'host\driver\Driver.cpp'; Pattern = 'IddCxSwapChainReleaseAndAcquireBuffer' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'D3D11_VIDEO_PROCESSOR' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'MFTEnumEx' },
    @{ Path = 'host\app\network_gate.cpp'; Pattern = 'NLM_NETWORK_CATEGORY_PRIVATE' },
    @{ Path = 'host\app\main.cpp'; Pattern = 'StartServiceCtrlDispatcherW' },
    @{ Path = 'host\app\pointer_relay.cpp'; Pattern = 'HarmonySecondaryScreen.Input' },
    @{ Path = 'host\app\input_agent.cpp'; Pattern = 'PointerInjector' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_CreateByMime' },
    @{ Path = 'receiver\entry\src\main\cpp\napi_init.cpp'; Pattern = 'OH_NativeXComponent_RegisterCallback' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'XComponentType.SURFACE' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'sessionShort' }
  )
  foreach ($check in $requiredPatterns) {
    & rg -q --fixed-strings $check.Pattern $check.Path
    if ($LASTEXITCODE -ne 0) { throw "Required implementation marker missing: $($check.Pattern) in $($check.Path)" }
  }

  Write-Host 'Static architecture and forbidden-dependency checks passed.'
} finally {
  Pop-Location
}
