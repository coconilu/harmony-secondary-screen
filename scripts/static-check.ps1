$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
  $parseTokens = $null
  $parseErrors = $null
  [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $projectRoot 'scripts\install-host-service.ps1'),
    [ref]$parseTokens,
    [ref]$parseErrors) | Out-Null
  if ($parseErrors.Count -ne 0) {
    throw "Host service installer contains PowerShell syntax errors:`n$($parseErrors | Out-String)"
  }
  & (Join-Path $projectRoot 'scripts\install-host-service.ps1') -ValidateFirewallLookup

  foreach ($relayScript in @(
    'scripts\build-relay.ps1',
    'scripts\install-relay-native-host.ps1',
    'scripts\send-receiver-smoke.ps1'
  )) {
    $relayTokens = $null
    $relayParseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
      (Join-Path $projectRoot $relayScript),
      [ref]$relayTokens,
      [ref]$relayParseErrors) | Out-Null
    if ($relayParseErrors.Count -ne 0) {
      throw "$relayScript contains PowerShell syntax errors:`n$($relayParseErrors | Out-String)"
    }
  }

  $forbidden = & rg -n -i '(@ohos\.web\.webview|\bWebView\b|<html|android\.intent|androidx\.|\.apk\b)' receiver 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a forbidden WebView/HTML5/Android dependency:`n$forbidden"
  }
  if ($LASTEXITCODE -ne 1) { throw "Forbidden dependency scan failed: $LASTEXITCODE" }

  $unsafeReceiver = & rg -n 'INADDR_ANY|0\.0\.0\.0|SetInputMode|OnTouch|\"pointer\"|\"scroll\"' `
    receiver\entry\src\main -g '*.cpp' -g '*.h' -g '*.ets' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a wildcard binding or forbidden input-return capability:`n$unsafeReceiver"
  }
  if ($LASTEXITCODE -ne 1) { throw "Receiver safety-boundary scan failed: $LASTEXITCODE" }

  $invalidIddType = & rg -n --fixed-strings 'IDDCX_MONITOR_TYPE_' host\driver 2>$null
  if ($LASTEXITCODE -eq 0) { throw "Invalid IddCx monitor type found:`n$invalidIddType" }
  if ($LASTEXITCODE -ne 1) { throw "IddCx invalid-type scan failed: $LASTEXITCODE" }

  $networkMutation = & rg -n 'SetCategory|NLM_NETWORK_CATEGORY_' host\app 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Host must not mutate or depend on the Windows network category:`n$networkMutation"
  }
  if ($LASTEXITCODE -ne 1) { throw "Network mutation scan failed: $LASTEXITCODE" }

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
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'IMFShutdown' },
    @{ Path = 'host\graphics\mf_h264_encoder.cpp'; Pattern = 'ContainsAvcCodecConfig' },
    @{ Path = 'host\app\local_security.h'; Pattern = 'GW;;;LS' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'PIPE_REJECT_REMOTE_CLIENTS' },
    @{ Path = 'host\app\network_gate.cpp'; Pattern = 'adapter->IfType != IF_TYPE_IEEE80211' },
    @{ Path = 'host\app\network_gate.cpp'; Pattern = 'AllowedWifiIpv4Addresses' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'IsAllowedWifiIpv4(localAddress' },
    @{ Path = 'host\app\main.cpp'; Pattern = 'ConfirmWifiAccess' },
    @{ Path = 'scripts\install-host-service.ps1'; Pattern = '-Profile Any -InterfaceType Wireless' },
    @{ Path = 'scripts\install-host-service.ps1'; Pattern = '-LocalPort 44000 -RemoteAddress LocalSubnet' },
    @{ Path = 'scripts\install-host-service.ps1'; Pattern = 'Remove-NetFirewallRule -ErrorAction Stop' },
    @{ Path = 'scripts\install-host-service.ps1'; Pattern = 'if ($Start) { Start-Service -Name $serviceName }' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.Revoke' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.CanSend' },
    @{ Path = 'host\app\host_server.cpp'; Pattern = 'data_plane_gate_.RunIfAllowed' },
    @{ Path = 'host\app\main.cpp'; Pattern = 'StartServiceCtrlDispatcherW' },
    @{ Path = 'host\app\pointer_relay.cpp'; Pattern = 'HarmonySecondaryScreen.Input' },
    @{ Path = 'host\app\input_agent.cpp'; Pattern = 'PointerInjector' },
    @{ Path = 'host\app\pointer_injector.cpp'; Pattern = 'QueryDisplayConfig' },
    @{ Path = 'host\app\pointer_injector.cpp'; Pattern = 'root#harmonysecondaryscreenidd' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_CreateByMime' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'IsCurrentWifiIpv4' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'tcpAddress.sin_addr = address' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'udpAddress.sin_addr = address' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'return type !=' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_Flush' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'OH_VideoDecoder_Start' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_lifecycle_mutex_' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_queue_mutex_' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'DecoderLifecycleState::kStopping' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'AVCODEC_BUFFER_FLAGS_CODEC_DATA' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'telemetry_queue_.Push' },
    @{ Path = 'receiver\entry\src\main\cpp\napi_init.cpp'; Pattern = 'OH_NativeXComponent_RegisterCallback' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'XComponentType.SURFACE' },
    @{ Path = 'receiver\entry\src\main\cpp\native_protocol.h'; Pattern = 'kVideoMagic = 0x48535332U' },
    @{ Path = 'receiver\entry\src\main\cpp\native_protocol.h'; Pattern = 'kVersion = 2' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'sessionShort' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'wifi_not_allowed' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'requireCodecConfig' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = '0x48574C31' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = '"receivedFrames"' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'htonl(INADDR_LOOPBACK)' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'SO_EXCLUSIVEADDRUSE' },
    @{ Path = 'relay\src\relay_protocol.cpp'; Pattern = 'BCryptGenRandom' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'expected_origin_' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'WebSocket continuation has no initial frame' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'fragmented WebSocket message exceeds local limit' },
    @{ Path = 'relay\src\relay_protocol.h'; Pattern = 'kMaxVideoPayloadBytes' },
    @{ Path = 'relay\src\lan_sender.cpp'; Pattern = 'BuildLanVideoDatagrams' },
    @{ Path = 'relay\src\lan_sender.cpp'; Pattern = 'IsTrustedLanIpv4' },
    @{ Path = 'relay\src\main.cpp'; Pattern = 'configure_receiver' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'SendAccessUnit' },
    @{ Path = 'relay\src\websocket_server.cpp'; Pattern = 'ConsumeKeyframeRequest' },
    @{ Path = 'scripts\install-relay-native-host.ps1'; Pattern = 'HKCU:\Software\Microsoft\Edge\NativeMessagingHosts' },
    @{ Path = 'scripts\install-relay-native-host.ps1'; Pattern = 'allowed_origins' },
    @{ Path = 'relay\tools\receiver_smoke_sender.mjs'; Pattern = 'VIDEO_MAGIC = 0x48535332' },
    @{ Path = 'scripts\send-receiver-smoke.ps1'; Pattern = 'color=c=0x0B111A:size=1280x720:rate=30' },
    @{ Path = 'extension\manifest.json'; Pattern = 'default_popup' },
    @{ Path = 'extension\service-worker.js'; Pattern = 'configure_receiver' },
    @{ Path = 'extension\offscreen.js'; Pattern = 'forceKeyFrame = true' }
  )
  foreach ($check in $requiredPatterns) {
    & rg -q --fixed-strings $check.Pattern $check.Path
    if ($LASTEXITCODE -ne 0) { throw "Required implementation marker missing: $($check.Pattern) in $($check.Path)" }
  }

  $extensionManifestPath = Join-Path $projectRoot 'extension\manifest.json'
  if (Test-Path -LiteralPath $extensionManifestPath) {
    $manifest = Get-Content -Raw -Encoding UTF8 $extensionManifestPath | ConvertFrom-Json
    if ($manifest.manifest_version -ne 3) {
      throw 'Edge capture probe must use Manifest V3.'
    }

    $permissions = @($manifest.permissions)
    foreach ($requiredPermission in @('activeTab', 'nativeMessaging', 'offscreen', 'storage', 'tabCapture')) {
      if ($permissions -notcontains $requiredPermission) {
        throw "Edge capture probe is missing permission: $requiredPermission"
      }
    }
    foreach ($forbiddenPermission in @('<all_urls>', 'cookies', 'history', 'webRequest', 'webRequestBlocking')) {
      if ($permissions -contains $forbiddenPermission) {
        throw "Edge capture probe requests forbidden permission: $forbiddenPermission"
      }
    }
    if ($null -ne $manifest.host_permissions -and @($manifest.host_permissions).Count -ne 0) {
      throw 'Edge capture probe must not request host_permissions.'
    }

    & rg -q --fixed-strings 'audio: false' extension\offscreen.js
    if ($LASTEXITCODE -ne 0) { throw 'Edge capture probe must explicitly disable audio capture.' }
    foreach ($encoderMarker in @(
      'VideoEncoder.isConfigSupported',
      'codec: "avc1.42001f"',
      'format: "annexb"',
      'width: ENCODE_WIDTH',
      'height: ENCODE_HEIGHT',
      'hardwareAcceleration: "prefer-hardware"'
    )) {
      $markerFound = Select-String -LiteralPath 'extension\offscreen.js' `
        -SimpleMatch -Quiet -Pattern $encoderMarker
      if (-not $markerFound) {
        throw "Edge encoding probe marker missing: $encoderMarker"
      }
    }
    $audioCapture = & rg -n 'audio\s*:\s*true|AudioContext|chrome\.cookies|chrome\.history' `
      extension -g '*.js' -g '*.json' 2>$null
    if ($LASTEXITCODE -eq 0) {
      throw "Edge capture probe contains a forbidden audio/privacy capability:`n$audioCapture"
    }
    if ($LASTEXITCODE -ne 1) { throw "Edge capture probe privacy scan failed: $LASTEXITCODE" }

    foreach ($relayMarker in @(
      'chrome.runtime.connectNative(NATIVE_RELAY_HOST)',
      'ws://127.0.0.1:${relayInfo.port}/capture',
      'LOCAL_RELAY_MAX_BUFFERED_BYTES',
      'chunk.copyTo(new Uint8Array(message, LOCAL_VIDEO_HEADER_SIZE))'
    )) {
      $markerFound = Select-String -LiteralPath `
        'extension\offscreen.js', `
        'extension\service-worker.js', `
        'extension\local-relay-protocol.js' `
        -SimpleMatch -Quiet -Pattern $relayMarker
      if (-not $markerFound) {
        throw "Edge local Relay marker missing: $relayMarker"
      }
    }
  }

  $unsafeRelayBinding = & rg -n 'INADDR_ANY|0\.0\.0\.0|HKLM:|Set-NetFirewall|New-NetFirewall' `
    relay\src scripts\install-relay-native-host.ps1 -g '*.cpp' -g '*.h' -g '*.ps1' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Relay contains a forbidden non-loopback/admin/network mutation marker:`n$unsafeRelayBinding"
  }
  if ($LASTEXITCODE -ne 1) { throw "Relay binding/admin scan failed: $LASTEXITCODE" }

  $lanListener = & rg -n '\bbind\s*\(|\blisten\s*\(' relay\src\lan_sender.cpp 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "LAN sender must only initiate outbound connections:`n$lanListener"
  }
  if ($LASTEXITCODE -ne 1) { throw "LAN sender outbound-only scan failed: $LASTEXITCODE" }

  $pairingPersistence = & rg -n 'chrome\.storage.*pairingCode|pairingCode.*chrome\.storage' `
    extension -g '*.js' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Pairing code must not be written to extension storage:`n$pairingPersistence"
  }
  if ($LASTEXITCODE -ne 1) { throw "Pairing-code persistence scan failed: $LASTEXITCODE" }

  Write-Host 'Legacy prototype and Edge capture/encoding/loopback-Relay static checks passed; real Edge testing remains separate.'
} finally {
  Pop-Location
}
