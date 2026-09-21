<#
 [Input] P4 tooling action, optional serial port, and optional ASCII PlatformIO core path.
 [Output] Reproducible UTF-8 P4 test/build/flash/factory/monitor workflows with guarded device writes.
 [Pos] Canonical Windows entry point for ESP32-P4 development and recovery.
 [Sync] If this file changes, update firmware/.folder.md and README.md.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet("doctor", "setup", "test", "build", "flash", "factory", "factory-flash", "monitor")]
    [string]$Action = "doctor",
    [string]$Port = "",
    [string]$PlatformIoCoreDir = "",
    [switch]$SkipBuild,
    [switch]$FactoryReset,
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $projectRoot
$environmentScript = Join-Path $repoRoot "pc\scripts\windows-utf8.ps1"

if (-not (Test-Path -LiteralPath $environmentScript)) {
    throw "Shared Windows environment helper is missing: $environmentScript"
}

. $environmentScript
Initialize-HachimoUtf8Environment -RepoRoot $repoRoot
$coreDir = Resolve-HachimoPlatformIoCoreDir `
    -RepoRoot $repoRoot `
    -RequestedPath $PlatformIoCoreDir
$python = Get-HachimoPythonPath
$buildDir = Join-Path $projectRoot ".pio\build\esp32_p4_evboard"

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][scriptblock]$Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

function Invoke-PlatformIo {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    Invoke-CheckedCommand -Label "PlatformIO" -Command {
        & $python -m platformio @Arguments
    }
}

function Invoke-ProtocolTests {
    Invoke-CheckedCommand -Label "P4 protocol tests" -Command {
        & $python (Join-Path $projectRoot "tests\protocol_contract_test.py")
    }
}

function Invoke-FirmwareBuild {
    $lockPath = Join-Path $projectRoot "dependencies.lock"
    $originalLock = $null
    if (Test-Path -LiteralPath $lockPath) {
        $originalLock = [System.IO.File]::ReadAllBytes($lockPath)
    }
    try {
        Invoke-PlatformIo -Arguments @("run", "-e", "esp32_p4_evboard")
    } finally {
        if ($null -ne $originalLock) {
            [System.IO.File]::WriteAllBytes($lockPath, $originalLock)
        }
    }
}

function Assert-FirmwareOutputs {
    foreach ($name in @("bootloader.bin", "firmware.bin", "partitions.bin", "ota_data_initial.bin")) {
        $path = Join-Path $buildDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Missing firmware build output: $path"
        }
    }
}

function Get-DevicePartitionLayout {
    param(
        [Parameter(Mandatory = $true)][string]$SerialPort,
        [Parameter(Mandatory = $true)][string]$Esptool
    )

    $safePort = $SerialPort -replace "[^A-Za-z0-9._-]", "_"
    $dumpPath = Join-Path $buildDir "device-partitions-$safePort.bin"
    $partitionTool = Join-Path $coreDir `
        "packages\framework-espidf\components\partition_table\gen_esp32part.py"
    if (-not (Test-Path -LiteralPath $partitionTool)) {
        throw "ESP-IDF partition tool was not found: $partitionTool"
    }

    Invoke-CheckedCommand -Label "Partition table read" -Command {
        & $Esptool --chip esp32p4 --port $SerialPort --baud 921600 `
            --connect-attempts 5 --before default-reset --after no-reset `
            read-flash 0x8000 0x1000 $dumpPath
    } | Out-Host

    $layout = (& $python $partitionTool $dumpPath | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to decode the device partition table."
    }
    return $layout
}

function Assert-CurrentAbLayout {
    param([Parameter(Mandatory = $true)][string]$Layout)

    $hasOta0 = $Layout -match "(?m)^ota_0,app,ota_0,0x10000,(2560K|0x280000),"
    $hasOta1 = $Layout -match "(?m)^ota_1,app,ota_1,0x290000,(2560K|0x280000),"
    $hasStorage = $Layout -match "(?m)^storage,data,spiffs,0x520000,(7040K|0x6E0000),"
    $hasAppearance0 = $Layout -match "(?m)^appearance0,data,64,0xc00000,(10M|0xA00000),"
    $hasAppearance1 = $Layout -match "(?m)^appearance1,data,65,0x1600000,(10M|0xA00000),"
    if (-not ($hasOta0 -and $hasOta1 -and $hasStorage -and $hasAppearance0 -and $hasAppearance1)) {
        throw "Device does not use the current dual-OTA + 640x480 appearance layout. Use factory-flash -FactoryReset."
    }
}

function Read-P4HelloVersion {
    param(
        [Parameter(Mandatory = $true)][string]$SerialPort,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion
    )

    Start-Sleep -Seconds 2
    $available = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($available -notcontains $SerialPort) {
        throw "$SerialPort did not re-enumerate after flashing."
    }

    $serial = [System.IO.Ports.SerialPort]::new(
        $SerialPort,
        4000000,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )
    $serial.ReadTimeout = 200
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    $serial.Encoding = [System.Text.Encoding]::UTF8
    $serial.Open()
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(18)
        $buffer = [System.Text.StringBuilder]::new()
        while ([DateTime]::UtcNow -lt $deadline) {
            $chunk = $serial.ReadExisting()
            if ($chunk) {
                [void]$buffer.Append($chunk)
                if ($buffer.ToString() -match '"fw":"([^"]+)"') {
                    $actual = $Matches[1]
                    if ($actual -ne $ExpectedVersion) {
                        throw "Device reported firmware $actual; expected $ExpectedVersion."
                    }
                    Write-Host "Verified device firmware $actual on $SerialPort."
                    return
                }
            }
            Start-Sleep -Milliseconds 100
        }
        throw "No P4 hello with a firmware version was received from $SerialPort."
    } finally {
        if ($serial.IsOpen) {
            $serial.Close()
        }
        $serial.Dispose()
    }
}

function Get-ProjectVersion {
    $cmake = Get-Content -LiteralPath (Join-Path $projectRoot "CMakeLists.txt") -Raw
    if ($cmake -notmatch 'set\(PROJECT_VER\s+"([^"]+)"\)') {
        throw "PROJECT_VER was not found in CMakeLists.txt."
    }
    return $Matches[1]
}

function Invoke-PreservingFlash {
    if (-not $Port.Trim()) {
        throw "flash requires -Port COMx."
    }
    if (-not $SkipBuild) {
        Invoke-FirmwareBuild
    }
    Assert-FirmwareOutputs

    $esptool = Get-HachimoEsptoolPath -PlatformIoCoreDir $coreDir
    $layout = Get-DevicePartitionLayout -SerialPort $Port -Esptool $esptool
    Assert-CurrentAbLayout -Layout $layout

    $firmware = Join-Path $buildDir "firmware.bin"
    Invoke-CheckedCommand -Label "Firmware image validation" -Command {
        & $esptool image-info $firmware
    }
    Invoke-CheckedCommand -Label "Firmware flash" -Command {
        & $esptool --chip esp32p4 --port $Port --baud 921600 `
            --connect-attempts 10 --before default-reset --after hard-reset `
            write-flash -z --flash-mode dio --flash-freq 80m --flash-size 32MB `
            0x2000 (Join-Path $buildDir "bootloader.bin") `
            0x10000 $firmware `
            0x8000 (Join-Path $buildDir "partitions.bin") `
            0x510000 (Join-Path $buildDir "ota_data_initial.bin")
    }

    if (-not $NoVerify) {
        Read-P4HelloVersion -SerialPort $Port -ExpectedVersion (Get-ProjectVersion)
    }
}

Push-Location $projectRoot
try {
    switch ($Action.ToLowerInvariant()) {
        "doctor" {
            Write-Host "Repository:      $repoRoot"
            Write-Host "Project:         $projectRoot"
            Write-Host "PlatformIO core: $coreDir"
            Write-Host "Temporary files: $env:TEMP"
            Write-Host "Python:          $python"
            Invoke-CheckedCommand -Label "PlatformIO version check" -Command {
                & $python -m platformio --version
            }
            Write-Host "UTF-8 and ASCII path checks passed."
        }
        "setup" {
            Set-HachimoUserToolEnvironment -PlatformIoCoreDir $coreDir
            Write-Host "Saved UTF-8 and PlatformIO settings for the current Windows user."
            Write-Host "New terminals will use: $coreDir"
        }
        "test" {
            Invoke-ProtocolTests
        }
        "build" {
            Invoke-ProtocolTests
            Invoke-FirmwareBuild
        }
        "flash" {
            Invoke-PreservingFlash
        }
        "factory" {
            Invoke-PlatformIo -Arguments @("run", "-e", "esp32_p4_evboard", "-t", "factory")
        }
        "factory-flash" {
            if (-not $FactoryReset) {
                throw "factory-flash erases device configuration, components, and appearances. Re-run with -FactoryReset."
            }
            if (-not $Port.Trim()) {
                throw "factory-flash requires -Port COMx."
            }
            Invoke-PlatformIo -Arguments @(
                "run",
                "-e",
                "esp32_p4_evboard",
                "-t",
                "factory_upload",
                "--upload-port",
                $Port
            )
        }
        "monitor" {
            if (-not $Port.Trim()) {
                throw "monitor requires -Port COMx."
            }
            Invoke-PlatformIo -Arguments @(
                "device",
                "monitor",
                "--port",
                $Port,
                "--baud",
                "4000000"
            )
        }
    }
} finally {
    Pop-Location
}
