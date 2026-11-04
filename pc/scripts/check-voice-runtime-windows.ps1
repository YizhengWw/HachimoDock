# Read-only installed-runtime preflight. No microphone, USB, service start or mutation.
param([string]$ResourceRoot = (Split-Path $PSScriptRoot -Parent), [switch]$CheckLoopback, [switch]$CheckLegacyVoice)
$ErrorActionPreference = 'Stop'
$report = [ordered]@{ platform='windows'; status='not_verified'; runtimeStarted=$false; deviceAccessed=$false; microphoneOpened=$false; windowsVoiceAcceptancePassed=$false }
try {
  if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) { throw 'Requires an actual Windows x64 machine' }
  $node = Join-Path $ResourceRoot 'bridge/runtime/node.exe'
  $verifier = Join-Path $ResourceRoot 'tools/voice-runtime-resources.mjs'
  if (-not (Test-Path -LiteralPath $node -PathType Leaf)) { throw 'Bundled Node executable missing' }
  if ($CheckLegacyVoice -and -not (Test-Path -LiteralPath $verifier -PathType Leaf)) { throw 'Bundled resource verifier missing' }
  # Native Node --version and a metadata-only verifier; never execute service entry.
  $report.nodeVersion = (& $node --version | Out-String).Trim()
  if ($LASTEXITCODE -ne 0) { throw 'Bundled Node cannot execute on this Windows machine' }
  # P4 native ASR does not use LiveKit8080 or the historical Node service.
  $report.p4NativeAsrRequiresLegacyService = $false
  $report.resources = @{enabled=$false;requiredForP4=$false;legacyChecked=$false}
  if ($CheckLegacyVoice) {
    $proof = & $node $verifier $ResourceRoot windows x64
    if ($LASTEXITCODE -ne 0) { throw 'Explicit legacy voice runtime integrity check failed' }
    $report.resources = ($proof | ConvertFrom-Json)
    if ($report.resources.enabled -eq $false) { throw 'Explicit legacy runtime was requested but is not bundled' }
  }
  if ($CheckLoopback) {
    $ports = @()
    foreach ($port in $(if($CheckLegacyVoice){@(8080,8181)}else{@(8181)})) {
      $client = [System.Net.Sockets.TcpClient]::new()
      try { $task=$client.ConnectAsync('127.0.0.1',$port); $ready=$task.Wait(1000); $ports += @{port=$port;tcpReachable=($ready -and $client.Connected)} }
      catch { $ports += @{port=$port;tcpReachable=$false} }
      finally { $client.Dispose() }
    }
    $report.loopback = $ports
  }
  $report.status = if ($report.resources.enabled -eq $false) { 'node_verified_optional_legacy_not_included' } else { 'installed_resources_and_node_verified_only' }
} catch { $report.status='failed'; $report.error=$_.Exception.Message }
$report | ConvertTo-Json -Depth 8
if ($report.status -eq 'failed') { exit 1 }
