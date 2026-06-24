param(
    [Parameter(Mandatory=$true)]
    [string]$HostName,
    [string]$RemoteDir = "/opt/board-runtime",
    [string]$SrcDir = "/opt/board-runtime-src",
    [string]$MqttUrl = "mqtt://broker.openclaw.example:1883",
    [switch]$ConfigureSpiLcd,
    [ValidateSet("ili9341", "st7789v")]
    [string]$LcdDriver = "ili9341",
    [int]$LcdDcPin = 15,
    [int]$LcdResetPin = 13,
    [int]$LcdBacklightPin = 0,
    [int]$LcdSpeedHz = 16000000,
    [int]$LcdRotate = 270,
    [switch]$LcdResetActiveHigh,
    [string]$SudoPassword = "",
    [switch]$SkipApt,
    [switch]$NoRestart
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Resolve-Path (Join-Path $ScriptDir "..")
$Archive = Join-Path ([System.IO.Path]::GetTempPath()) ("board-runtime-radxa-{0}.tgz" -f ([System.Guid]::NewGuid().ToString("N")))
$RemoteArchive = "/tmp/board-runtime-radxa.tgz"

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $Name"
    }
}

function Format-ShSingleQuoted {
    param([string]$Value)
    return "'" + $Value.Replace("'", "'\''") + "'"
}

function Get-RemoteSudoPrefix {
    if ([string]::IsNullOrEmpty($SudoPassword)) {
        return ""
    }
    $quoted = Format-ShSingleQuoted $SudoPassword
    return "SUDO_PASSWORD=$quoted; sudo() { printf '%s\n' `"`$SUDO_PASSWORD`" | command sudo -S -p '' `"`$@`"; }; "
}

function Invoke-Remote {
    param([string]$Command)
    $remoteCommand = (Get-RemoteSudoPrefix) + $Command
    & ssh -o BatchMode=yes -o StrictHostKeyChecking=no $HostName $remoteCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Remote command failed with exit code $LASTEXITCODE"
    }
}

function Invoke-RemoteScript {
    param([string]$Script)
    if (-not [string]::IsNullOrEmpty($SudoPassword)) {
        $Script = (Get-RemoteSudoPrefix) + "`n" + $Script
    }
    $Script | & ssh -o BatchMode=yes -o StrictHostKeyChecking=no $HostName "tr -d '\r' | sh -s"
    if ($LASTEXITCODE -ne 0) {
        throw "Remote script failed with exit code $LASTEXITCODE"
    }
}

Require-Command ssh
Require-Command scp
Require-Command tar

try {
    Write-Host "==> Probing $HostName"
    Invoke-Remote 'hostname; uname -m; . /etc/os-release && echo "$PRETTY_NAME"'

    Write-Host "==> Creating source archive"
    if (Test-Path $Archive) {
        Remove-Item -LiteralPath $Archive -Force
    }
    & tar -czf $Archive `
        --exclude ".git" `
        --exclude ".claude" `
        --exclude ".venv" `
        --exclude ".DS_Store" `
        --exclude "._*" `
        --exclude "__pycache__" `
        --exclude "*/__pycache__" `
        --exclude "build" `
        --exclude "build-*" `
        --exclude "*.o" `
        -C $ProjectDir "."
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed with exit code $LASTEXITCODE"
    }

    Write-Host "==> Uploading archive to ${HostName}:$RemoteArchive"
    & scp -q -o BatchMode=yes -o StrictHostKeyChecking=no $Archive "${HostName}:$RemoteArchive"
    if ($LASTEXITCODE -ne 0) {
        throw "scp failed with exit code $LASTEXITCODE"
    }

    Write-Host "==> Extracting source on device"
    Invoke-RemoteScript @"
set -eu
sudo rm -rf '$SrcDir'
sudo mkdir -p '$SrcDir'
sudo tar -xzf '$RemoteArchive' -C '$SrcDir' --no-same-owner
sudo chown -R `$(id -un):`$(id -gn) '$SrcDir'
rm -f '$RemoteArchive'
"@

    if (-not $SkipApt) {
        Write-Host "==> Installing build/runtime dependencies"
        Invoke-RemoteScript @'
set -eu
for i in $(seq 1 180); do
    if command -v fuser >/dev/null 2>&1; then
        if sudo fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/lib/apt/lists/lock /var/cache/apt/archives/lock >/dev/null 2>&1; then
            sleep 2
        else
            break
        fi
    elif pgrep -x apt-get >/dev/null 2>&1 || pgrep -x apt >/dev/null 2>&1 || pgrep -x dpkg >/dev/null 2>&1 || pgrep -x unattended-upgr >/dev/null 2>&1; then
        sleep 2
    else
        break
    fi
done
sudo apt-get update -qq
sudo apt-get install -y -qq cmake gcc make zlib1g-dev iw python3 python3-pip python3-pil python3-numpy ffmpeg flac alsa-utils fonts-wqy-zenhei fonts-noto-color-emoji
python3 -m pip install --quiet --retries 5 --timeout 60 SpeechRecognition || true
'@
    }

    Write-Host "==> Building on device"
    Invoke-Remote "cd '$SrcDir' && cmake -S . -B build -DBOARD_RUNTIME_BUILD_TESTS=OFF && cmake --build build -j`$(nproc)"

    Write-Host "==> Installing runtime files"
    $InstallScript = @'
set -eu
SRC_DIR='__SRC_DIR__'
REMOTE_DIR='__REMOTE_DIR__'
MQTT_URL='__MQTT_URL__'

sudo mkdir -p "$REMOTE_DIR"
sudo systemctl stop board-runtime board-widget-runtime.service 2>/dev/null || true

for bin in board-server board-touch-input board-rotary-input fb-speech-overlay board-serial-bridge; do
    sudo cp "$SRC_DIR/build/$bin" "$REMOTE_DIR/"
done

for f in board-ap-up.sh board-ap-down.sh board-sta-apply.sh board-network-watchdog.sh board-wifi-scan.sh board-selfcheck.sh board-audio-bridge.sh board-sound.sh fb-device.sh fb-display.sh fb-rawvideo-blit.py fb-stats-renderer.py board-widget-runtime.py board-voice-ptt.py start-rpi.sh board-widget-runtime.service; do
    [ -f "$SRC_DIR/$f" ] && sudo cp "$SRC_DIR/$f" "$REMOTE_DIR/"
done

sudo chmod +x "$REMOTE_DIR"/*.sh "$REMOTE_DIR"/board-server "$REMOTE_DIR"/board-touch-input "$REMOTE_DIR"/board-rotary-input "$REMOTE_DIR"/fb-speech-overlay "$REMOTE_DIR"/board-serial-bridge 2>/dev/null || true
sudo find "$REMOTE_DIR" -maxdepth 1 -type f \( -name "*.sh" -o -name "*.py" -o -name "*.service" -o -name "*.env" \) -exec sed -i 's/\r$//' {} \;

[ -d "$SRC_DIR/ui" ] && sudo rm -rf "$REMOTE_DIR/ui" && sudo cp -r "$SRC_DIR/ui" "$REMOTE_DIR/"
[ -d "$SRC_DIR/assets" ] && sudo rm -rf "$REMOTE_DIR/assets" && sudo cp -r "$SRC_DIR/assets" "$REMOTE_DIR/" || true
[ -d "$SRC_DIR/builtin-clawpkgs" ] && sudo rm -rf "$REMOTE_DIR/builtin-clawpkgs" && sudo cp -r "$SRC_DIR/builtin-clawpkgs" "$REMOTE_DIR/" || true
[ -f "$SRC_DIR/unifont-17.0.04.hex.gz" ] && sudo cp "$SRC_DIR/unifont-17.0.04.hex.gz" "$REMOTE_DIR/" && cd "$REMOTE_DIR" && sudo ln -sf unifont-17.0.04.hex.gz unifont.hex.gz || true

if [ -d "$REMOTE_DIR/assets/pets/terrier/generated-videos" ]; then
    sudo rm -rf "$REMOTE_DIR/terrier-clips"
    sudo mkdir -p "$REMOTE_DIR/terrier-clips"
    for src in "$REMOTE_DIR"/assets/pets/terrier/generated-videos/*/*.loop.raw.mp4; do
        [ -f "$src" ] || continue
        name=$(basename "$src" .loop.raw.mp4)
        sudo ln -sf "$src" "$REMOTE_DIR/terrier-clips/$name.mp4"
    done
fi

sudo tee "$REMOTE_DIR/board-runtime-radxa.env" >/dev/null <<EOF
BOARD_DIR=/opt/board-runtime
MQTT_BROKER_URL=$MQTT_URL
BOARD_TRANSPORT_FORCE=mqtt
BOARD_RUNTIME_WLAN_IFACE=wlan0
BOARD_RUNTIME_HOST=0.0.0.0
BOARD_RUNTIME_PORT=80
BOARD_RUNTIME_DISCOVERY_UDP_PORT=19890
BOARD_RUNTIME_DISCOVERY_MDNS_PORT=5353
BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS=0
BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS=3000
PET_CLAW_FB_DEV=auto
PET_ROTARY_INPUT_ENABLED=0
PET_VOICE_PTT_ENABLED=0
PET_VOICE_BUTTON_ENABLED=0
PET_CLAW_MQTT_NAMESPACE=desk
EOF

sudo tee "$REMOTE_DIR/start-radxa-a733.sh" >/dev/null <<'EOF'
#!/bin/sh
set -eu

BOARD_DIR="${BOARD_DIR:-/opt/board-runtime}"
MQTT_BROKER_URL="${MQTT_BROKER_URL:-mqtt://broker.openclaw.example:1883}"
BOARD_RUNTIME_WLAN_IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"

wait_net=0
while [ ! -d "/sys/class/net/${BOARD_RUNTIME_WLAN_IFACE}" ] && [ "$wait_net" -lt 20 ]; do
    sleep 1
    wait_net=$((wait_net + 1))
done

if [ -z "${PET_DEVICE_ID:-}" ]; then
    WLAN_MAC=$(cat "/sys/class/net/${BOARD_RUNTIME_WLAN_IFACE}/address" 2>/dev/null | tr -d ':' | tr 'A-F' 'a-f')
    if [ -n "$WLAN_MAC" ]; then
        PET_DEVICE_ID="board-${WLAN_MAC}"
    else
        PET_DEVICE_ID="board-unknown"
    fi
fi

export BOARD_DIR
export MQTT_URL="$MQTT_BROKER_URL"
export PET_CLAW_MQTT_URL="$MQTT_BROKER_URL"
export PET_DEVICE_ID
export PET_CLAW_DEVICE_ID="$PET_DEVICE_ID"
export PET_SCREEN_NAME="${PET_SCREEN_NAME:-OpenClaw Board Runtime}"
export PET_CLAW_MQTT_NAMESPACE="${PET_CLAW_MQTT_NAMESPACE:-desk}"
export BOARD_RUNTIME_HOST="${BOARD_RUNTIME_HOST:-0.0.0.0}"
export BOARD_RUNTIME_PORT="${BOARD_RUNTIME_PORT:-80}"
export BOARD_RUNTIME_DISCOVERY_UDP_PORT="${BOARD_RUNTIME_DISCOVERY_UDP_PORT:-19890}"
export BOARD_RUNTIME_DISCOVERY_MDNS_PORT="${BOARD_RUNTIME_DISCOVERY_MDNS_PORT:-5353}"
export BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS="${BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS:-0}"
export BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS="${BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS:-3000}"
export BOARD_TRANSPORT="${BOARD_TRANSPORT_FORCE:-mqtt}"

if [ -f "$BOARD_DIR/fb-device.sh" ]; then
    . "$BOARD_DIR/fb-device.sh"
fi

FB_REQUEST="${PET_CLAW_FB_DEV:-auto}"
FB_DEV=""
if command -v fb_resolve_device >/dev/null 2>&1; then
    FB_DEV="$(fb_resolve_device "$FB_REQUEST" 2>/dev/null || true)"
else
    case "$FB_REQUEST" in
        ""|auto|AUTO) [ -c /dev/fb0 ] && FB_DEV=/dev/fb0 || true ;;
        *) [ -c "$FB_REQUEST" ] && FB_DEV="$FB_REQUEST" || true ;;
    esac
fi
export PET_CLAW_FB_DEV="$FB_DEV"

PIDS=""
cleanup() {
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    wait
}
trap cleanup INT TERM

"$BOARD_DIR/board-server" "$BOARD_DIR" &
PIDS="$PIDS $!"

if [ -n "$FB_DEV" ] && [ -c "$FB_DEV" ] && [ -f "$BOARD_DIR/fb-display.sh" ]; then
    sh "$BOARD_DIR/fb-display.sh" &
    PIDS="$PIDS $!"
fi

wait
EOF
sudo chmod +x "$REMOTE_DIR/start-radxa-a733.sh"

sudo tee /etc/systemd/system/board-runtime.service >/dev/null <<'EOF'
[Unit]
Description=Board Runtime (claw-pet device, Radxa A733)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=/opt/board-runtime/board-runtime-radxa.env
ExecStart=/bin/sh /opt/board-runtime/start-radxa-a733.sh
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

[ -f "$REMOTE_DIR/board-widget-runtime.service" ] && sudo cp "$REMOTE_DIR/board-widget-runtime.service" /etc/systemd/system/board-widget-runtime.service

needs_config=1
if [ -f "$REMOTE_DIR/network-config.json" ]; then
    python3 -c 'import json, sys; data=json.load(open(sys.argv[1])); sys.exit(0 if data.get("ssid") else 1)' "$REMOTE_DIR/network-config.json" >/dev/null 2>&1 && needs_config=0 || needs_config=1
fi
if [ "$needs_config" = 1 ]; then
    ssid=$(nmcli -t -f active,ssid dev wifi 2>/dev/null | awk -F: '$1 == "yes" {print $2; exit}')
    if [ -n "$ssid" ]; then
        SSID="$ssid" MQTT_URL_VALUE="$MQTT_URL" MQTT_NAMESPACE_VALUE="desk" python3 -c 'import json, os; print(json.dumps({"ssid": os.environ["SSID"], "mqttUrl": os.environ["MQTT_URL_VALUE"], "mqttNamespace": os.environ["MQTT_NAMESPACE_VALUE"]}, ensure_ascii=False))' | sudo tee "$REMOTE_DIR/network-config.json" >/dev/null
        sudo chmod 600 "$REMOTE_DIR/network-config.json"
    fi
fi

sudo systemctl daemon-reload
sudo systemctl enable board-runtime board-widget-runtime.service >/dev/null
'@
    $InstallScript = $InstallScript.Replace("__SRC_DIR__", $SrcDir).Replace("__REMOTE_DIR__", $RemoteDir).Replace("__MQTT_URL__", $MqttUrl)
    Invoke-RemoteScript $InstallScript

    $rebootingForLcd = $false
    if ($ConfigureSpiLcd) {
        Write-Host "==> Configuring Radxa A7Z SPI LCD overlay"
        $lcdScript = Join-Path $ScriptDir "configure-radxa-a733-spi-lcd.ps1"
        $lcdArgs = @(
            "-HostName", $HostName,
            "-Driver", $LcdDriver,
            "-SpiBus", "spi1",
            "-ChipSelect", "0",
            "-DcPin", "$LcdDcPin",
            "-ResetPin", "$LcdResetPin",
            "-BacklightPin", "$LcdBacklightPin",
            "-SpeedHz", "$LcdSpeedHz",
            "-Rotate", "$LcdRotate"
        )
        if (-not [string]::IsNullOrEmpty($SudoPassword)) {
            $lcdArgs += @("-SudoPassword", $SudoPassword)
        }
        if ($LcdResetActiveHigh) {
            $lcdArgs += "-ResetActiveHigh"
        }
        if (-not $NoRestart) {
            $lcdArgs += "-Reboot"
            $rebootingForLcd = $true
        }
        & powershell -NoProfile -ExecutionPolicy Bypass -File $lcdScript @lcdArgs
        if ($LASTEXITCODE -ne 0) {
            throw "SPI LCD configuration failed with exit code $LASTEXITCODE"
        }
    }

    if ($rebootingForLcd) {
        Write-Host "==> Reboot requested for LCD overlay; wait for the board to come back before checking service status"
    } elseif (-not $NoRestart) {
        Write-Host "==> Restarting services"
        Invoke-Remote "sudo systemctl restart board-runtime board-widget-runtime.service"
    }

    if (-not $rebootingForLcd) {
        Write-Host "==> Service status"
        Invoke-Remote 'for i in $(seq 1 30); do if systemctl is-active --quiet board-runtime && curl -fsS --max-time 2 http://127.0.0.1/board-runtime-config.json >/dev/null; then break; fi; sleep 1; done; systemctl is-active board-runtime; systemctl is-active board-widget-runtime.service; curl -fsS --max-time 5 http://127.0.0.1/board-runtime-config.json >/dev/null && echo http-ok'
    }
    Write-Host "==> Done"
}
finally {
    if (Test-Path $Archive) {
        Remove-Item -LiteralPath $Archive -Force
    }
}
