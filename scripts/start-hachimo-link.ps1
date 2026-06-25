<#
 [Input] Local network adapter state plus optional board SSH target.
 [Output] Starts the local MQTT broker and bridge with a reachable LAN broker URL, optionally syncing the board runtime.
 [Pos] script node in scripts.
 [Sync] If this file changes, update `scripts/.folder.md`.
#>
param(
    [string]$MqttHost = "0.0.0.0",
    [int]$MqttPort = 1883,
    [string]$LanIp = "",
    [int]$BridgePort = 23333,
    [string]$BoardHost = "",
    [string]$SudoPassword = "",
    [switch]$UpdateBoard
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BridgeDir = Join-Path $RepoRoot "ref\src-tauri\bridge\packages\clawd-backend-service"
$BrokerLog = Join-Path $RepoRoot "dev-mqtt-broker.log"
$BrokerErr = Join-Path $RepoRoot "dev-mqtt-broker.err.log"
$BridgeLog = Join-Path $RepoRoot "dev-bridge.log"
$BridgeErr = Join-Path $RepoRoot "dev-bridge.err.log"

function Get-DefaultLanIPv4 {
    $defaultRoutes = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Where-Object { $_.NextHop -and $_.NextHop -ne "0.0.0.0" } |
        Sort-Object RouteMetric, InterfaceMetric

    foreach ($route in $defaultRoutes) {
        $address = Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue |
            Where-Object {
                $_.IPAddress -and
                $_.IPAddress -notlike "127.*" -and
                $_.IPAddress -notlike "169.254.*"
            } |
            Select-Object -First 1
        if ($address) {
            return $address.IPAddress
        }
    }

    $fallback = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object {
            $_.IPAddress -and
            $_.IPAddress -notlike "127.*" -and
            $_.IPAddress -notlike "169.254.*"
        } |
        Select-Object -First 1
    if ($fallback) {
        return $fallback.IPAddress
    }

    throw "Could not detect a LAN IPv4 address. Pass -LanIp <pc-ip> explicitly."
}

function Stop-ListenerOnPort([int]$Port) {
    Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty OwningProcess -Unique |
        ForEach-Object {
            if ($_ -and $_ -ne $PID) {
                Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue
            }
        }
}

$ResolvedLanIp = $LanIp.Trim()
if (-not $ResolvedLanIp) {
    $ResolvedLanIp = Get-DefaultLanIPv4
}
$MqttUrl = "mqtt://$ResolvedLanIp`:$MqttPort"

Stop-ListenerOnPort $MqttPort
Stop-ListenerOnPort $BridgePort
Start-Sleep -Milliseconds 500

$env:MQTT_LISTEN_HOST = $MqttHost
$env:MQTT_LISTEN_PORT = "$MqttPort"
Start-Process -FilePath "node" `
    -ArgumentList @("scripts/dev-mqtt-broker.js") `
    -WorkingDirectory $RepoRoot `
    -WindowStyle Hidden `
    -RedirectStandardOutput $BrokerLog `
    -RedirectStandardError $BrokerErr | Out-Null

Start-Sleep -Seconds 2

$env:MQTT_URL = $MqttUrl
$env:STATUS_NAMESPACE = "desk"
$env:CLAWD_BRIDGE_PORT = "$BridgePort"
$env:CLAWD_ENABLE_CODEX_MONITOR = "1"
$env:AGENT_BUS_DISABLED = "1"
Start-Process -FilePath "node" `
    -ArgumentList @("src/headless-mqtt.js") `
    -WorkingDirectory $BridgeDir `
    -WindowStyle Hidden `
    -RedirectStandardOutput $BridgeLog `
    -RedirectStandardError $BridgeErr | Out-Null

Start-Sleep -Seconds 3

Write-Host "Resolved LAN IP: $ResolvedLanIp"
Write-Host "MQTT URL: $MqttUrl"

Write-Host "MQTT broker:"
Get-NetTCPConnection -State Listen -LocalPort $MqttPort -ErrorAction SilentlyContinue |
    Select-Object LocalAddress, LocalPort, OwningProcess |
    Format-Table -AutoSize

Write-Host "Bridge:"
Get-NetTCPConnection -State Listen -LocalPort $BridgePort -ErrorAction SilentlyContinue |
    Select-Object LocalAddress, LocalPort, OwningProcess |
    Format-Table -AutoSize

Write-Host "Bridge health:"
try {
    (Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 "http://127.0.0.1:$BridgePort/state").Content
} catch {
    Write-Host $_.Exception.Message
}

if ($UpdateBoard) {
    if (-not $BoardHost.Trim()) {
        throw "Pass -BoardHost <user@board-ip> with -UpdateBoard."
    }

    $remoteScript = @'
set -eu
run_sudo() {
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        printf '%s\n' "$SUDO_PASSWORD" | sudo -S "$@"
    else
        sudo "$@"
    fi
}
REMOTE_DIR="${BOARD_RUNTIME_DIR:-/opt/board-runtime}"
MQTT_URL_VALUE="__MQTT_URL__"
ENV_FILE="$REMOTE_DIR/board-runtime-radxa.env"
if [ -f "$ENV_FILE" ]; then
    if grep -q '^MQTT_BROKER_URL=' "$ENV_FILE"; then
        run_sudo sed -i "s#^MQTT_BROKER_URL=.*#MQTT_BROKER_URL=$MQTT_URL_VALUE#" "$ENV_FILE"
    else
        printf '%s\n' "MQTT_BROKER_URL=$MQTT_URL_VALUE" | run_sudo tee -a "$ENV_FILE" >/dev/null
    fi
fi
if [ -f "$REMOTE_DIR/network-config.json" ]; then
    tmp="$(mktemp)"
    MQTT_URL_VALUE="$MQTT_URL_VALUE" python3 - "$REMOTE_DIR/network-config.json" > "$tmp" <<'PY'
import json
import os
import sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as fh:
    data = json.load(fh)
data["mqttUrl"] = os.environ["MQTT_URL_VALUE"]
print(json.dumps(data, ensure_ascii=False))
PY
    run_sudo install -m 600 "$tmp" "$REMOTE_DIR/network-config.json"
    rm -f "$tmp"
fi
run_sudo systemctl restart board-runtime
systemctl is-active board-runtime
'@
    $remoteScript = $remoteScript.Replace("__MQTT_URL__", $MqttUrl)
    $escapedPassword = $SudoPassword.Replace("'", "'\''")
    $remoteCommand = if ($SudoPassword) {
        "SUDO_PASSWORD='$escapedPassword' sh -s"
    } else {
        "sh -s"
    }

    Write-Host "Updating board runtime MQTT URL on $BoardHost..."
    $remoteScript | ssh $BoardHost $remoteCommand
}
