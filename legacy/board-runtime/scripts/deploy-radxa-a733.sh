#!/bin/sh
# Deploy board-runtime to Radxa Cubie A7Z / A733 Debian images.
# Usage:
#   HOST=radxa@<board-ip> sh scripts/deploy-radxa-a733.sh
#   HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
set -eu

if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST is required, for example: HOST=radxa@<board-ip> sh scripts/deploy-radxa-a733.sh" >&2
    exit 1
fi
REMOTE_DIR="${REMOTE_DIR:-/opt/board-runtime}"
SRC_DIR="${SRC_DIR:-/opt/board-runtime-src}"
MQTT_URL="${MQTT_URL:-mqtt://broker.openclaw.example:1883}"
SKIP_APT="${SKIP_APT:-0}"
NO_RESTART="${NO_RESTART:-0}"
CONFIGURE_SPI_LCD="${CONFIGURE_SPI_LCD:-0}"

LCD_DRIVER="${LCD_DRIVER:-ili9341}"
LCD_DC_PIN="${LCD_DC_PIN:-15}"
LCD_RESET_PIN="${LCD_RESET_PIN:-13}"
LCD_BACKLIGHT_PIN="${LCD_BACKLIGHT_PIN:-0}"
LCD_SPEED_HZ="${LCD_SPEED_HZ:-16000000}"
LCD_ROTATE="${LCD_ROTATE:-270}"
LCD_RESET_ACTIVE_HIGH="${LCD_RESET_ACTIVE_HIGH:-0}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCHIVE="${TMPDIR:-/tmp}/board-runtime-radxa-$$.tgz"
REMOTE_ARCHIVE="/tmp/board-runtime-radxa.tgz"

shell_quote() {
    printf "%s" "$1" | sed "s/'/'\\\\''/g"
}

remote_prefix() {
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        sudo_password_quoted=$(shell_quote "$SUDO_PASSWORD")
        printf "SUDO_PASSWORD='%s'; sudo() { printf '%%s\\n' \"\$SUDO_PASSWORD\" | command sudo -S -p '' \"\$@\"; }; " "$sudo_password_quoted"
    fi
}

remote() {
    prefix="$(remote_prefix)"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$HOST" "$prefix$1"
}

remote_script() {
    prefix="$(remote_prefix)"
    {
        [ -z "$prefix" ] || printf '%s\n' "$prefix"
        cat
    } | ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$HOST" "tr -d '\r' | sh -s"
}

cleanup() {
    rm -f "$ARCHIVE"
}
trap cleanup EXIT INT TERM

wait_for_apt() {
    remote "for i in \$(seq 1 180); do if command -v fuser >/dev/null 2>&1; then if sudo fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/lib/apt/lists/lock /var/cache/apt/archives/lock >/dev/null 2>&1; then sleep 2; else exit 0; fi; elif pgrep -x apt-get >/dev/null 2>&1 || pgrep -x apt >/dev/null 2>&1 || pgrep -x dpkg >/dev/null 2>&1 || pgrep -x unattended-upgr >/dev/null 2>&1; then sleep 2; else exit 0; fi; done; echo 'apt lock still busy after wait' >&2; exit 1"
}

configure_spi_lcd() {
    echo "==> Configuring Radxa A7Z SPI LCD overlay"
    case "$LCD_DRIVER" in
        ili9341) compatible="ilitek,ili9341" ;;
        st7789v) compatible="sitronix,st7789v" ;;
        *) echo "ERROR: LCD_DRIVER must be ili9341 or st7789v" >&2; exit 1 ;;
    esac

    if [ "${LCD_CHIP_SELECT:-0}" != "0" ]; then
        echo "ERROR: A7Z physical pin 26 is PD14/SPI1-HOLD, not Raspberry Pi CE1. Use LCD_CHIP_SELECT=0." >&2
        exit 1
    fi

    gpio_ref() {
        case "$1" in
            13) printf '<&r_pio 0 6 %s>' "${2:-0}" ;;
            15) printf '<&r_pio 0 7 %s>' "${2:-0}" ;;
            32) printf '<&r_pio 1 5 %s>' "${2:-0}" ;;
            *) echo "ERROR: unsupported A7Z LCD GPIO physical pin $1 in deploy-radxa-a733.sh" >&2; exit 1 ;;
        esac
    }

    reset_flags=1
    if [ "$LCD_RESET_ACTIVE_HIGH" = "1" ]; then
        reset_flags=0
    fi
    dc_gpio="$(gpio_ref "$LCD_DC_PIN" 0)"
    reset_gpio="$(gpio_ref "$LCD_RESET_PIN" "$reset_flags")"
    if [ "$LCD_BACKLIGHT_PIN" = "0" ]; then
        backlight_line=""
    else
        backlight_gpio="$(gpio_ref "$LCD_BACKLIGHT_PIN" 0)"
        backlight_line="                led-gpios = $backlight_gpio;"
    fi

    remote_script <<EOF
set -eu
workdir=\$(mktemp -d)
trap 'rm -rf "\$workdir"' EXIT
dts="\$workdir/radxa-a7z-spi28-rpi-pins-$LCD_DRIVER.dtbo.dts"
dtbo="/boot/dtbo/radxa-a7z-spi28-rpi-pins-$LCD_DRIVER.dtbo"
sudo rm -f /boot/dtbo/codex-radxa-a733-spi*-lcd.dtbo /boot/dtbo/codex-radxa-a733-spi*-lcd.dtbo.disabled

cat >"\$dts" <<'DTS'
/dts-v1/;
/plugin/;

/ {
    metadata {
        title = "OpenClaw Radxa A7Z SPI LCD";
        compatible = "radxa,cubie-a7a", "radxa,cubie-a7z", "radxa,cubie-a7s";
        category = "display";
        exclusive = "spi1,PD10,PD11,PD12,PD13,PIN_13,PIN_15";
        description = "Enable a Raspberry Pi-style 2.8 inch SPI LCD framebuffer for board-runtime.";
    };

    fragment@0 {
        target = <&pio>;
        __overlay__ {
            spi1_lcd_pins: spi1@0 {
                pins = "PD11", "PD12", "PD13";
                function = "spi1";
                drive-strength = <10>;
            };

            spi1_lcd_cs: spi1@1 {
                pins = "PD10";
                function = "spi1";
                drive-strength = <10>;
                bias-pull-up;
            };

            spi1_lcd_sleep: spi1@2 {
                pins = "PD10", "PD11", "PD12", "PD13";
                function = "gpio_in";
                drive-strength = <10>;
            };
        };
    };

    fragment@1 {
        target = <&spi1>;
        __overlay__ {
            #address-cells = <1>;
            #size-cells = <0>;
            clock-frequency = <50000000>;
            pinctrl-0 = <&spi1_lcd_pins &spi1_lcd_cs>;
            pinctrl-1 = <&spi1_lcd_sleep>;
            pinctrl-names = "default", "sleep";
            sunxi,spi-bus-mode = <1>;
            sunxi,spi-cs-mode = <0>;
            status = "okay";

            spidev0 {
                status = "disabled";
            };

            spidev1 {
                status = "disabled";
            };

            display@0 {
                compatible = "$compatible";
                reg = <0>;
                spi-max-frequency = <$LCD_SPEED_HZ>;
                width = <240>;
                height = <320>;
                regwidth = <8>;
                rotate = <$LCD_ROTATE>;
                fps = <60>;
                buswidth = <8>;
                bpp = <16>;
                txbuflen = <32768>;
                debug = <0>;
                bgr;
                dc-gpios = $dc_gpio;
                reset-gpios = $reset_gpio;
$backlight_line
                status = "okay";
            };
        };
    };
};
DTS

sudo dtc -@ -I dts -O dtb -o "\$dtbo" "\$dts"
sudo u-boot-update
echo "installed \$dtbo"
echo "driver=$LCD_DRIVER spi=spi1 chip_select=0 dc_pin=$LCD_DC_PIN reset_pin=$LCD_RESET_PIN reset_active_low=$([ "$LCD_RESET_ACTIVE_HIGH" = "1" ] && printf false || printf true) backlight_pin=$LCD_BACKLIGHT_PIN speed=$LCD_SPEED_HZ rotate=$LCD_ROTATE"
EOF
}

echo "==> Probing $HOST"
remote 'hostname; uname -m; . /etc/os-release && echo "$PRETTY_NAME"'

echo "==> Creating source archive"
rm -f "$ARCHIVE"
tar -czf "$ARCHIVE" \
    --exclude ".git" \
    --exclude ".claude" \
    --exclude ".venv" \
    --exclude ".DS_Store" \
    --exclude "._*" \
    --exclude "__pycache__" \
    --exclude "*/__pycache__" \
    --exclude "build" \
    --exclude "build-*" \
    --exclude "*.o" \
    -C "$PROJECT_DIR" "."

echo "==> Uploading archive to $HOST:$REMOTE_ARCHIVE"
scp -q -o BatchMode=yes -o StrictHostKeyChecking=no "$ARCHIVE" "$HOST:$REMOTE_ARCHIVE"

echo "==> Extracting source on device"
remote_script <<EOF
set -eu
sudo rm -rf '$SRC_DIR'
sudo mkdir -p '$SRC_DIR'
sudo tar -xzf '$REMOTE_ARCHIVE' -C '$SRC_DIR' --no-same-owner
sudo chown -R \$(id -un):\$(id -gn) '$SRC_DIR'
rm -f '$REMOTE_ARCHIVE'
EOF

if [ "$SKIP_APT" != "1" ]; then
    echo "==> Installing build/runtime dependencies"
    wait_for_apt
    remote "sudo apt-get update -qq && sudo apt-get install -y -qq cmake gcc make zlib1g-dev iw python3 python3-pip python3-pil python3-numpy ffmpeg flac alsa-utils fonts-wqy-zenhei fonts-noto-color-emoji"
    remote "python3 -m pip install --quiet --retries 5 --timeout 60 SpeechRecognition || true"
fi

echo "==> Building on device"
remote "cd '$SRC_DIR' && cmake -S . -B build -DBOARD_RUNTIME_BUILD_TESTS=OFF && cmake --build build -j\$(nproc)"

echo "==> Installing runtime files"
remote_script <<EOF
set -eu
SRC_DIR='$SRC_DIR'
REMOTE_DIR='$REMOTE_DIR'
MQTT_URL='$MQTT_URL'

sudo mkdir -p "\$REMOTE_DIR"
sudo systemctl stop board-runtime board-widget-runtime.service 2>/dev/null || true

for bin in board-server board-touch-input board-rotary-input fb-speech-overlay board-serial-bridge; do
    sudo cp "\$SRC_DIR/build/\$bin" "\$REMOTE_DIR/"
done

for f in board-ap-up.sh board-ap-down.sh board-sta-apply.sh board-network-watchdog.sh board-wifi-scan.sh board-selfcheck.sh board-audio-bridge.sh board-sound.sh fb-device.sh fb-display.sh fb-rawvideo-blit.py fb-stats-renderer.py board-widget-runtime.py board-voice-ptt.py start-rpi.sh board-widget-runtime.service; do
    [ -f "\$SRC_DIR/\$f" ] && sudo cp "\$SRC_DIR/\$f" "\$REMOTE_DIR/"
done

sudo chmod +x "\$REMOTE_DIR"/*.sh "\$REMOTE_DIR"/board-server "\$REMOTE_DIR"/board-touch-input "\$REMOTE_DIR"/board-rotary-input "\$REMOTE_DIR"/fb-speech-overlay "\$REMOTE_DIR"/board-serial-bridge 2>/dev/null || true
sudo find "\$REMOTE_DIR" -maxdepth 1 -type f \( -name "*.sh" -o -name "*.py" -o -name "*.service" -o -name "*.env" \) -exec sed -i 's/\r$//' {} \;

[ -d "\$SRC_DIR/ui" ] && sudo rm -rf "\$REMOTE_DIR/ui" && sudo cp -r "\$SRC_DIR/ui" "\$REMOTE_DIR/"
[ -d "\$SRC_DIR/assets" ] && sudo rm -rf "\$REMOTE_DIR/assets" && sudo cp -r "\$SRC_DIR/assets" "\$REMOTE_DIR/" || true
[ -d "\$SRC_DIR/builtin-clawpkgs" ] && sudo rm -rf "\$REMOTE_DIR/builtin-clawpkgs" && sudo cp -r "\$SRC_DIR/builtin-clawpkgs" "\$REMOTE_DIR/" || true
[ -f "\$SRC_DIR/unifont-17.0.04.hex.gz" ] && sudo cp "\$SRC_DIR/unifont-17.0.04.hex.gz" "\$REMOTE_DIR/" && cd "\$REMOTE_DIR" && sudo ln -sf unifont-17.0.04.hex.gz unifont.hex.gz || true

if [ -d "\$REMOTE_DIR/assets/pets/terrier/generated-videos" ]; then
    sudo rm -rf "\$REMOTE_DIR/terrier-clips"
    sudo mkdir -p "\$REMOTE_DIR/terrier-clips"
    for src in "\$REMOTE_DIR"/assets/pets/terrier/generated-videos/*/*.loop.raw.mp4; do
        [ -f "\$src" ] || continue
        name=\$(basename "\$src" .loop.raw.mp4)
        sudo ln -sf "\$src" "\$REMOTE_DIR/terrier-clips/\$name.mp4"
    done
fi

sudo tee "\$REMOTE_DIR/board-runtime-radxa.env" >/dev/null <<ENV
BOARD_DIR=$REMOTE_DIR
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
ENV

sudo tee "\$REMOTE_DIR/start-radxa-a733.sh" >/dev/null <<'START'
#!/bin/sh
set -eu

BOARD_DIR="\${BOARD_DIR:-/opt/board-runtime}"
MQTT_BROKER_URL="\${MQTT_BROKER_URL:-mqtt://broker.openclaw.example:1883}"
BOARD_RUNTIME_WLAN_IFACE="\${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"

wait_net=0
while [ ! -d "/sys/class/net/\${BOARD_RUNTIME_WLAN_IFACE}" ] && [ "\$wait_net" -lt 20 ]; do
    sleep 1
    wait_net=\$((wait_net + 1))
done

if [ -z "\${PET_DEVICE_ID:-}" ]; then
    WLAN_MAC=\$(cat "/sys/class/net/\${BOARD_RUNTIME_WLAN_IFACE}/address" 2>/dev/null | tr -d ':' | tr 'A-F' 'a-f')
    if [ -n "\$WLAN_MAC" ]; then
        PET_DEVICE_ID="board-\${WLAN_MAC}"
    else
        PET_DEVICE_ID="board-unknown"
    fi
fi

export BOARD_DIR
export MQTT_URL="\$MQTT_BROKER_URL"
export PET_CLAW_MQTT_URL="\$MQTT_BROKER_URL"
export PET_DEVICE_ID
export PET_CLAW_DEVICE_ID="\$PET_DEVICE_ID"
export PET_SCREEN_NAME="\${PET_SCREEN_NAME:-OpenClaw Board Runtime}"
export PET_CLAW_MQTT_NAMESPACE="\${PET_CLAW_MQTT_NAMESPACE:-desk}"
export BOARD_RUNTIME_HOST="\${BOARD_RUNTIME_HOST:-0.0.0.0}"
export BOARD_RUNTIME_PORT="\${BOARD_RUNTIME_PORT:-80}"
export BOARD_RUNTIME_DISCOVERY_UDP_PORT="\${BOARD_RUNTIME_DISCOVERY_UDP_PORT:-19890}"
export BOARD_RUNTIME_DISCOVERY_MDNS_PORT="\${BOARD_RUNTIME_DISCOVERY_MDNS_PORT:-5353}"
export BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS="\${BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS:-0}"
export BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS="\${BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS:-3000}"
export BOARD_TRANSPORT="\${BOARD_TRANSPORT_FORCE:-mqtt}"

if [ -f "\$BOARD_DIR/fb-device.sh" ]; then
    . "\$BOARD_DIR/fb-device.sh"
fi

FB_REQUEST="\${PET_CLAW_FB_DEV:-auto}"
FB_DEV=""
if command -v fb_resolve_device >/dev/null 2>&1; then
    FB_DEV="\$(fb_resolve_device "\$FB_REQUEST" 2>/dev/null || true)"
else
    case "\$FB_REQUEST" in
        ""|auto|AUTO) [ -c /dev/fb0 ] && FB_DEV=/dev/fb0 || true ;;
        *) [ -c "\$FB_REQUEST" ] && FB_DEV="\$FB_REQUEST" || true ;;
    esac
fi
export PET_CLAW_FB_DEV="\$FB_DEV"

PIDS=""
cleanup() {
    for pid in \$PIDS; do
        kill "\$pid" 2>/dev/null || true
    done
    wait
}
trap cleanup INT TERM

"\$BOARD_DIR/board-server" "\$BOARD_DIR" &
PIDS="\$PIDS \$!"

if [ -n "\$FB_DEV" ] && [ -c "\$FB_DEV" ] && [ -f "\$BOARD_DIR/fb-display.sh" ]; then
    sh "\$BOARD_DIR/fb-display.sh" &
    PIDS="\$PIDS \$!"
fi

wait
START
sudo chmod +x "\$REMOTE_DIR/start-radxa-a733.sh"

sudo tee /etc/systemd/system/board-runtime.service >/dev/null <<UNIT
[Unit]
Description=Board Runtime (claw-pet device, Radxa A733)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=$REMOTE_DIR/board-runtime-radxa.env
ExecStart=/bin/sh $REMOTE_DIR/start-radxa-a733.sh
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNIT

[ -f "\$REMOTE_DIR/board-widget-runtime.service" ] && sudo cp "\$REMOTE_DIR/board-widget-runtime.service" /etc/systemd/system/board-widget-runtime.service

needs_config=1
if [ -f "\$REMOTE_DIR/network-config.json" ]; then
    python3 -c 'import json, sys; data=json.load(open(sys.argv[1])); sys.exit(0 if data.get("ssid") else 1)' "\$REMOTE_DIR/network-config.json" >/dev/null 2>&1 && needs_config=0 || needs_config=1
fi
if [ "\$needs_config" = 1 ]; then
    ssid=\$(nmcli -t -f active,ssid dev wifi 2>/dev/null | awk -F: '\$1 == "yes" {print \$2; exit}')
    if [ -n "\$ssid" ]; then
        SSID="\$ssid" MQTT_URL_VALUE="\$MQTT_URL" MQTT_NAMESPACE_VALUE="desk" python3 -c 'import json, os; print(json.dumps({"ssid": os.environ["SSID"], "mqttUrl": os.environ["MQTT_URL_VALUE"], "mqttNamespace": os.environ["MQTT_NAMESPACE_VALUE"]}, ensure_ascii=False))' | sudo tee "\$REMOTE_DIR/network-config.json" >/dev/null
        sudo chmod 600 "\$REMOTE_DIR/network-config.json"
    fi
fi

sudo systemctl daemon-reload
sudo systemctl enable board-runtime board-widget-runtime.service >/dev/null
EOF

rebooting_for_lcd=0
if [ "$CONFIGURE_SPI_LCD" = "1" ]; then
    configure_spi_lcd
    if [ "$NO_RESTART" != "1" ]; then
        rebooting_for_lcd=1
        remote "(sleep 1; sudo reboot) >/dev/null 2>&1 &"
    fi
fi

if [ "$rebooting_for_lcd" = "1" ]; then
    echo "==> Reboot requested for LCD overlay; wait for the board to come back before checking service status"
elif [ "$NO_RESTART" != "1" ]; then
    echo "==> Restarting services"
    remote "sudo systemctl restart board-runtime board-widget-runtime.service"
    echo "==> Service status"
    remote 'for i in $(seq 1 30); do if systemctl is-active --quiet board-runtime && curl -fsS --max-time 2 http://127.0.0.1/board-runtime-config.json >/dev/null; then break; fi; sleep 1; done; systemctl is-active board-runtime; systemctl is-active board-widget-runtime.service; curl -fsS --max-time 5 http://127.0.0.1/board-runtime-config.json >/dev/null && echo http-ok'
fi

echo "==> Done"
