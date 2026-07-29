$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
  $forbiddenReceiver = & rg -n -i '(@ohos\.web\.webview|\bWebView\b|<html|android\.intent|androidx\.|\.apk\b)' receiver 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a forbidden WebView/HTML5/Android dependency:`n$forbiddenReceiver"
  }
  if ($LASTEXITCODE -ne 1) { throw "Receiver dependency scan failed: $LASTEXITCODE" }

  $unsafeReceiver = & rg -n 'INADDR_ANY|0\.0\.0\.0|SetInputMode|OnTouch|\"pointer\"|\"scroll\"' `
    receiver\entry\src\main -g '*.cpp' -g '*.h' -g '*.ets' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Receiver contains a wildcard binding or forbidden input-return capability:`n$unsafeReceiver"
  }
  if ($LASTEXITCODE -ne 1) { throw "Receiver safety-boundary scan failed: $LASTEXITCODE" }

  $manifest = Get-Content -Raw -Encoding UTF8 extension\manifest.json | ConvertFrom-Json
  if ($manifest.manifest_version -ne 3) { throw 'Edge extension must use Manifest V3.' }
  $permissions = @($manifest.permissions)
  foreach ($required in @('activeTab', 'offscreen', 'storage', 'tabCapture', 'webRequest')) {
    if ($permissions -notcontains $required) { throw "Extension is missing permission: $required" }
  }
  foreach ($forbidden in @('nativeMessaging', '<all_urls>', 'cookies', 'history', 'scripting',
                            'webRequestBlocking')) {
    if ($permissions -contains $forbidden) { throw "Extension requests forbidden permission: $forbidden" }
  }
  if (@($manifest.host_permissions).Count -ne 1 -or
      @($manifest.host_permissions)[0] -ne 'http://harmony-web-companion.local/*') {
    throw 'Persistent host permission must be limited to harmony-web-companion.local.'
  }
  if (@($manifest.optional_host_permissions).Count -ne 1 -or
      @($manifest.optional_host_permissions)[0] -ne 'http://*/*') {
    throw 'Manual IPv4 fallback must use one optional HTTP origin declaration.'
  }

  $legacyRuntime = & rg -n 'connectNative|127\.0\.0\.1.*capture|startLocalRelay|connectLocalRelay' `
    extension scripts\test.ps1 README.md docs\ARCHITECTURE.md docs\PROTOCOL.md extension\README.md 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Normal user path still calls the Relay or Native Host:`n$legacyRuntime"
  }
  if ($LASTEXITCODE -ne 1) { throw "Relay dependency scan failed: $LASTEXITCODE" }

  $privacyViolation = & rg -n 'audio\s*:\s*true|AudioContext|chrome\.cookies|chrome\.history|document\.body\.inner' `
    extension -g '*.js' -g '*.json' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Extension contains a forbidden audio or page-data capability:`n$privacyViolation"
  }
  if ($LASTEXITCODE -ne 1) { throw "Extension privacy scan failed: $LASTEXITCODE" }

  foreach ($check in @(
    @{ Path = 'extension\offscreen.js'; Pattern = 'audio: false' },
    @{ Path = 'extension\offscreen.js'; Pattern = 'new DirectReceiverConnection' },
    @{ Path = 'extension\direct-delivery-orchestrator.js'; Pattern = 'createDirectVideoMessage' },
    @{ Path = 'extension\offscreen.js'; Pattern = 'beginReceiverRecovery' },
    @{ Path = 'extension\offscreen.js'; Pattern = 'directReconnecting' },
    @{ Path = 'extension\direct-client.js'; Pattern = 'DIRECT_RECOVERY_MAX_RETRY_DELAY_MS' },
    @{ Path = 'extension\direct-client.js'; Pattern = 'ReceiverRecoveryCancelledError' },
    @{ Path = 'extension\direct-client.js'; Pattern = 'async recover' },
    @{ Path = 'extension\direct-client.js'; Pattern = 'this.authenticated = true' },
    @{ Path = 'extension\direct-resync-policy.js'; Pattern = 'requireKeyFrame' },
    @{ Path = 'extension\direct-resync-policy.js'; Pattern = 'canDeliverEncodedChunk' },
    @{ Path = 'extension\direct-delivery-orchestrator.js'; Pattern = 'deliver(chunk, connection, sourceEpoch, telemetry)' },
    @{ Path = 'extension\offscreen.js'; Pattern = 'directDelivery.deliver' },
    @{ Path = 'extension\tests\direct-websocket.integration.test.js'; Pattern = 'recovers an abnormal drop without replacing the capture source' },
    @{ Path = 'extension\direct-protocol.js'; Pattern = 'PAIRING_TTL_MS = 60_000' },
    @{ Path = 'extension\direct-protocol.js'; Pattern = 'sourceEpoch' },
    @{ Path = 'extension\pending-pairing-store.js'; Pattern = 'chrome.storage.session' },
    @{ Path = 'extension\setup.js'; Pattern = 'await updatePendingHost(host)' },
    @{ Path = 'extension\host-permissions.js'; Pattern = 'api.request({ origins: [origin] })' },
    @{ Path = 'extension\host-permissions.js'; Pattern = 'const granted = await api.getAll()' },
    @{ Path = 'extension\host-permissions.js'; Pattern = 'if (!removed)' },
    @{ Path = 'extension\setup.js'; Pattern = 'cleanupUnusedManualHostPermissions([])' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'IsCurrentWifiIpv4' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'tcpAddress.sin_addr = address' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'std::strncmp(item->ifa_name, "wlan", 4)' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'if_nametoindex' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'address_responder_.Stop()' },
    @{ Path = 'receiver\entry\src\main\cpp\mdns_posix_transport.cpp'; Pattern = 'IP_PKTINFO' },
    @{ Path = 'receiver\entry\src\main\cpp\mdns_posix_transport.cpp'; Pattern = 'IP_RECVTTL' },
    @{ Path = 'receiver\entry\src\main\cpp\mdns_posix_transport.cpp'; Pattern = 'IP_MULTICAST_ALL' },
    @{ Path = 'receiver\entry\src\main\cpp\mdns_posix_transport.cpp'; Pattern = 'recvmsg' },
    @{ Path = 'receiver\entry\src\main\cpp\mdns_responder.cpp'; Pattern = 'SendGoodbyeLocked' },
    @{ Path = 'receiver\tests\mdns_protocol_tests.cpp'; Pattern = 'AddressChangeAndStopRevokeTheOldRecord' },
    @{ Path = 'receiver\tests\mdns_responder_tests.cpp'; Pattern = 'EstablishedOwnerDefeatsALaterStarter' },
    @{ Path = 'receiver\tests\mdns_responder_tests.cpp'; Pattern = 'SimultaneousProbesUseDeterministicTieBreak' },
    @{ Path = 'receiver\tests\mdns_responder_tests.cpp'; Pattern = 'WrongInterfacePortHopAndDestinationAreIgnored' },
    @{ Path = 'receiver\tests\mdns_responder_tests.cpp'; Pattern = 'AddressInvalidationSendsExactlyOneGoodbye' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'authorization_replayed' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'identity_mismatch' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'IsAcceptedSourceEpoch' },
    @{ Path = 'receiver\entry\src\main\cpp\decoder_orchestration.h'; Pattern = 'BeginFlush' },
    @{ Path = 'receiver\entry\src\main\cpp\decoder_orchestration.h'; Pattern = 'DecoderRecoveryCoordinator' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_callback_gate_.BeginFlush' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'decoder_recovery_.Admit' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'scanBarcode.startScanForResult' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'mdns.addLocalService' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'preferences.getPreferences' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'PLAYBACK_ACTIVE_EVENT' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'Scroll()' },
    @{ Path = 'receiver\entry\src\main\ets\entryability\EntryAbility.ets'; Pattern = 'setWindowKeepScreenOn' },
    @{ Path = 'receiver\entry\src\main\ets\entryability\EntryAbility.ets'; Pattern = 'onWindowStageDestroy' },
    @{ Path = 'receiver\entry\src\main\ets\entryability\EntryAbility.ets'; Pattern = 'onAppForeground' },
    @{ Path = 'receiver\entry\src\main\ets\entryability\EntryAbility.ets'; Pattern = 'onAppBackground' },
    @{ Path = 'receiver\entry\src\main\cpp\napi_init.cpp'; Pattern = 'OnSurfaceDestroyed(component, window)' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'ApplyLifecycleDecisionLocked' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'lifecycle_state_.SurfaceDestroyed' },
    @{ Path = 'receiver\tests\receiver_lifecycle_tests.cpp'; Pattern = 'TestNewSurfaceCreatedBeforeOldDestroyed' },
    @{ Path = 'receiver\tests\receiver_lifecycle_tests.cpp'; Pattern = 'TestForegroundBackgroundSurfaceInterleaving' },
    @{ Path = 'receiver\tests\receiver_lifecycle_tests.cpp'; Pattern = 'TestSameEpochAuthenticationRequiresCompleteRecovery' },
    @{ Path = 'receiver\tests\receiver_lifecycle_tests.cpp'; Pattern = 'TestFlushClosesNeedInputGateBeforeClearingQueues' },
    @{ Path = 'receiver\tests\receiver_lifecycle_tests.cpp'; Pattern = 'TestDecodeQueueOverflowInvalidatesDependencies' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'HWC4' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'sourceEpoch' }
  )) {
    $found = Select-String -LiteralPath $check.Path -SimpleMatch -Quiet -Pattern $check.Pattern
    if (-not $found) {
      throw "Required direct implementation marker missing: $($check.Pattern) in $($check.Path)"
    }
  }

  $pairingPersistence = & rg -n 'chrome\.storage.*(token|sessionId|shortCode)|((token|sessionId|shortCode).*)chrome\.storage' `
    extension -g '*.js' 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "One-time pairing authorization must not be persisted:`n$pairingPersistence"
  }
  if ($LASTEXITCODE -ne 1) { throw "Pairing persistence scan failed: $LASTEXITCODE" }

  $pendingLocalPersistence = & rg -n 'chrome\.storage\.local|storage\.local' `
    extension\pending-pairing-store.js 2>$null
  if ($LASTEXITCODE -eq 0) {
    throw "Pending pairing authorization must use session storage only:`n$pendingLocalPersistence"
  }
  if ($LASTEXITCODE -ne 1) { throw "Pending storage boundary scan failed: $LASTEXITCODE" }

  $candidateCredentialFiles = & rg --files `
    -g '!out/**' `
    -g '!extension/node_modules/**' `
    -g '!receiver/node_modules/**' `
    -g '!receiver/**/.cxx/**' `
    -g '!receiver/**/build/**'
  if ($LASTEXITCODE -ne 0) { throw "Credential file inventory failed: $LASTEXITCODE" }
  $trackedCredentials = $candidateCredentialFiles | ForEach-Object {
    if ((Test-Path -LiteralPath $_) -and (Get-Item -LiteralPath $_).Length -lt 5MB) {
      Select-String -LiteralPath $_ -Pattern @(
        '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----',
        '(?i)"(keyPassword|storePassword)"\s*:\s*"[^"]+"',
        '(?i)"(certpath|storeFile|profile)"\s*:\s*"[A-Za-z]:\\Users\\'
      ) -ErrorAction SilentlyContinue
    }
  }
  if ($trackedCredentials) {
    throw "Tracked files contain credentials or local signing paths:`n$($trackedCredentials | Out-String)"
  }

  Write-Host 'Direct Receiver, permission, privacy, binding, and credential static checks passed.'
} finally {
  Pop-Location
}

exit 0
