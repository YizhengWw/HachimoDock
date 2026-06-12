param(
    [Parameter(Mandatory=$true)]
    [string]$HostName,
    [ValidateSet("ili9341", "st7789v")]
    [string]$Driver = "ili9341",
    [ValidateSet("spi1", "spi3")]
    [string]$SpiBus = "spi1",
    [int]$DcPin = 15,
    [int]$ResetPin = 13,
    [int]$BacklightPin = 0,
    [ValidateSet(0, 1)]
    [int]$ChipSelect = 0,
    [int]$SpeedHz = 16000000,
    [int]$Rotate = 90,
    [switch]$ResetActiveLow,
    [switch]$ResetActiveHigh,
    [string]$SudoPassword = "",
    [switch]$Reboot
)

$ErrorActionPreference = "Stop"

# Defaults match the 2.8 inch Raspberry Pi-style SPI LCD when it is wired to
# Radxa Cubie A7Z physical pins:
#   CS=24/PD10, CLK=23/PD11, MOSI=19/PD12, MISO=21/PD13,
#   RES=13/PL6 active-low, DC=15/PL7, BLK tied directly to 3.3V.
# Physical pin 26 is PD14/SPI1-HOLD on A7Z, not a Raspberry Pi-compatible CE1.

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $Name"
    }
}

function Invoke-RemoteScript {
    param([string]$Script)
    $Script | & ssh -o BatchMode=yes -o StrictHostKeyChecking=no $HostName "tr -d '\r' | bash -s"
    if ($LASTEXITCODE -ne 0) {
        throw "Remote script failed with exit code $LASTEXITCODE"
    }
}

function Get-GpioSpec {
    param([int]$PhysicalPin)

    $map = @{
        3  = @{ Controller = "pio";   Bank = 9;  Pin = 23 }
        5  = @{ Controller = "pio";   Bank = 9;  Pin = 22 }
        7  = @{ Controller = "pio";   Bank = 1;  Pin = 0  }
        8  = @{ Controller = "pio";   Bank = 1;  Pin = 9  }
        10 = @{ Controller = "pio";   Bank = 1;  Pin = 10 }
        11 = @{ Controller = "pio";   Bank = 1;  Pin = 1  }
        12 = @{ Controller = "pio";   Bank = 1;  Pin = 5  }
        13 = @{ Controller = "r_pio"; Bank = 0;  Pin = 6  }
        15 = @{ Controller = "r_pio"; Bank = 0;  Pin = 7  }
        16 = @{ Controller = "pio";   Bank = 9;  Pin = 24 }
        18 = @{ Controller = "pio";   Bank = 9;  Pin = 25 }
        22 = @{ Controller = "r_pio"; Bank = 0;  Pin = 5  }
        26 = @{ Controller = "pio";   Bank = 3;  Pin = 14 }
        27 = @{ Controller = "pio";   Bank = 3;  Pin = 17 }
        28 = @{ Controller = "pio";   Bank = 3;  Pin = 16 }
        29 = @{ Controller = "pio";   Bank = 1;  Pin = 2  }
        31 = @{ Controller = "pio";   Bank = 1;  Pin = 3  }
        32 = @{ Controller = "r_pio"; Bank = 1;  Pin = 5  }
        33 = @{ Controller = "r_pio"; Bank = 1;  Pin = 3  }
        35 = @{ Controller = "pio";   Bank = 1;  Pin = 6  }
        36 = @{ Controller = "pio";   Bank = 1;  Pin = 4  }
        37 = @{ Controller = "r_pio"; Bank = 1;  Pin = 4  }
        38 = @{ Controller = "pio";   Bank = 1;  Pin = 8  }
        40 = @{ Controller = "pio";   Bank = 1;  Pin = 7  }
    }

    if (-not $map.ContainsKey($PhysicalPin)) {
        throw "Physical pin $PhysicalPin is not in this script's GPIO map. Use a free 40-pin GPIO such as 22, 18, 12, 13, or 15."
    }
    return $map[$PhysicalPin]
}

function Format-GpioRef {
    param(
        [hashtable]$Spec,
        [int]$Flags = 0
    )
    return "<&$($Spec.Controller) $($Spec.Bank) $($Spec.Pin) $Flags>"
}

function Format-ShSingleQuoted {
    param([string]$Value)
    return "'" + $Value.Replace("'", "'\''") + "'"
}

Require-Command ssh

if ($ResetActiveLow.IsPresent -and $ResetActiveHigh.IsPresent) {
    throw "Use only one of -ResetActiveLow or -ResetActiveHigh."
}

$dc = Format-GpioRef (Get-GpioSpec $DcPin)
$resetIsActiveLow = -not $ResetActiveHigh.IsPresent
$resetFlags = if ($resetIsActiveLow) { 1 } else { 0 }
$reset = Format-GpioRef (Get-GpioSpec $ResetPin) $resetFlags
$backlightLine = if ($BacklightPin -gt 0) {
    $backlight = Format-GpioRef (Get-GpioSpec $BacklightPin)
    "                led-gpios = $backlight;"
} else {
    ""
}

if ($SpiBus -eq "spi1") {
    if ($ChipSelect -ne 0) {
        throw "Cubie A7Z physical pin 26 is PD14/SPI1-HOLD, not a Raspberry Pi-compatible SPI1 CS1. Use -ChipSelect 0 for LCD CS on physical pin 24."
    }
    $spiCsPin = "PD10"
    $spiPins = @"
            spi1_pins_default: spi1@0 {
                pins = "PD11", "PD12", "PD13";
                function = "spi1";
                drive-strength = <10>;
            };

            spi1_pins_cs: spi1@1 {
                pins = "$spiCsPin";
                function = "spi1";
                drive-strength = <10>;
                bias-pull-up;
            };

            spi1_pins_sleep: spi1@2 {
                pins = "PD11", "PD12", "PD13", "$spiCsPin";
                function = "gpio_in";
                drive-strength = <10>;
            };
"@
    $pinctrlDefault = "<&spi1_pins_default &spi1_pins_cs>"
    $pinctrlSleep = "<&spi1_pins_sleep>"
    $exclusive = "spi1,$spiCsPin,PD11,PD12,PD13"
} else {
    if ($ChipSelect -ne 0) {
        throw "This script maps only chip select 0 for SPI LCDs."
    }
    $spiPins = @"
            spi3_pins_default: spi3@0 {
                pins = "PK5", "PK6", "PK7";
                function = "spi3";
                drive-strength = <10>;
            };

            spi3_pins_cs: spi3@1 {
                pins = "PK8";
                function = "spi3";
                drive-strength = <10>;
                bias-pull-up;
            };

            spi3_pins_sleep: spi3@2 {
                pins = "PK5", "PK6", "PK7", "PK8";
                function = "gpio_in";
                drive-strength = <10>;
            };
"@
    $pinctrlDefault = "<&spi3_pins_default &spi3_pins_cs>"
    $pinctrlSleep = "<&spi3_pins_sleep>"
    $exclusive = "spi3,PK5,PK6,PK7,PK8"
}

$compatible = if ($Driver -eq "ili9341") { "ilitek,ili9341" } else { "sitronix,st7789v" }
$overlayName = "radxa-a7z-spi28-rpi-pins-$Driver.dtbo"
$rebootValue = if ($Reboot) { "1" } else { "0" }
$sudoPasswordLiteral = Format-ShSingleQuoted $SudoPassword

$script = @"
set -euo pipefail
SUDO_PASSWORD=$sudoPasswordLiteral

run_sudo() {
    if [ -n "`$SUDO_PASSWORD" ]; then
        printf '%s\n' "`$SUDO_PASSWORD" | sudo -S -p '' "`$@"
    else
        sudo "`$@"
    fi
}

workdir=`$(mktemp -d)
trap 'rm -rf "`$workdir"' EXIT
dts="`$workdir/$overlayName.dts"
dtbo="/boot/dtbo/$overlayName"
run_sudo rm -f /boot/dtbo/codex-radxa-a733-spi*-lcd.dtbo /boot/dtbo/codex-radxa-a733-spi*-lcd.dtbo.disabled

cat >"`$dts" <<'DTS'
/dts-v1/;
/plugin/;

/ {
    metadata {
        title = "Codex Radxa A733 SPI LCD $Driver on $SpiBus";
        compatible = "radxa,cubie-a7a", "radxa,cubie-a7z", "radxa,cubie-a7s";
        category = "display";
        exclusive = "$exclusive";
        description = "Enable a 2.8 inch SPI LCD framebuffer for board-runtime.";
    };

    fragment@0 {
        target = <&pio>;
        __overlay__ {
$spiPins
        };
    };

    fragment@1 {
        target = <&$SpiBus>;
        __overlay__ {
            #address-cells = <1>;
            #size-cells = <0>;
            clock-frequency = <50000000>;
            pinctrl-0 = $pinctrlDefault;
            pinctrl-1 = $pinctrlSleep;
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

            display@$ChipSelect {
                compatible = "$compatible";
                reg = <$ChipSelect>;
                spi-max-frequency = <$SpeedHz>;
                width = <240>;
                height = <320>;
                regwidth = <8>;
                rotate = <$Rotate>;
                fps = <60>;
                buswidth = <8>;
                bpp = <16>;
                txbuflen = <32768>;
                debug = <0>;
                bgr;
                dc-gpios = $dc;
                reset-gpios = $reset;
$backlightLine
                status = "okay";
            };
        };
    };
};
DTS

run_sudo dtc -@ -I dts -O dtb -o "`$dtbo" "`$dts"
run_sudo u-boot-update
echo "installed `$dtbo"
echo "driver=$Driver spi=$SpiBus chip_select=$ChipSelect dc_pin=$DcPin reset_pin=$ResetPin reset_active_low=$resetIsActiveLow backlight_pin=$BacklightPin speed=$SpeedHz rotate=$Rotate"
if [ "$rebootValue" = "1" ]; then
    echo "rebooting"
    (sleep 1; run_sudo reboot) >/dev/null 2>&1 &
else
    echo "reboot required"
fi
"@

Invoke-RemoteScript $script
