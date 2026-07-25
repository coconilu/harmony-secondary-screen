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
  foreach ($required in @('activeTab', 'offscreen', 'storage', 'tabCapture')) {
    if ($permissions -notcontains $required) { throw "Extension is missing permission: $required" }
  }
  foreach ($forbidden in @('nativeMessaging', '<all_urls>', 'cookies', 'history', 'scripting',
                            'webRequest', 'webRequestBlocking')) {
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
    @{ Path = 'extension\offscreen.js'; Pattern = 'createDirectVideoMessage' },
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
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'authorization_replayed' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'identity_mismatch' },
    @{ Path = 'receiver\entry\src\main\cpp\receiver_session.cpp'; Pattern = 'IsAcceptedSourceEpoch' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'scanBarcode.startScanForResult' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'mdns.addLocalService' },
    @{ Path = 'receiver\entry\src\main\ets\pages\Index.ets'; Pattern = 'preferences.getPreferences' },
    @{ Path = 'docs\PROTOCOL.md'; Pattern = 'HWC3' },
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
