param(
  [string]$Port = "COM5",
  [string]$BackupPath = "",
  [switch]$SkipBuild,
  [switch]$FactoryReset,
  [string]$PlatformIoCoreDir = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $projectRoot
$environmentScript = Join-Path $repoRoot "pc\scripts\windows-utf8.ps1"

. $environmentScript
Initialize-HachimoUtf8Environment -RepoRoot $repoRoot
$coreDir = Resolve-HachimoPlatformIoCoreDir `
  -RepoRoot $repoRoot `
  -RequestedPath $PlatformIoCoreDir
$python = Get-HachimoPythonPath
$buildDir = Join-Path $projectRoot ".pio\build\esp32_p4_evboard"
$esptool = Get-HachimoEsptoolPath -PlatformIoCoreDir $coreDir
$partitionTool = Join-Path $coreDir "packages\framework-espidf\components\partition_table\gen_esp32part.py"
$partitionDump = Join-Path $env:TEMP "pet-manager-p4-partitions-$PID.bin"

if (-not $BackupPath) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $BackupPath = Join-Path $env:TEMP "pet-manager-p4-spiffs-before-ab-$stamp.bin"
}
$BackupPath = [System.IO.Path]::GetFullPath($BackupPath)

if (-not (Test-Path -LiteralPath $partitionTool)) {
  throw "ESP-IDF partition tool not found: $partitionTool"
}

Push-Location $projectRoot
try {
  if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "p4.ps1") build -PlatformIoCoreDir $coreDir
    if ($LASTEXITCODE -ne 0) { throw "Guarded P4 build failed" }
  }

  foreach ($name in @("bootloader.bin", "firmware.bin", "partitions.bin", "ota_data_initial.bin")) {
    $path = Join-Path $buildDir $name
    if (-not (Test-Path -LiteralPath $path)) { throw "missing build output: $path" }
  }

  & $esptool --chip esp32p4 --port $Port --before default-reset --after no-reset `
    read-flash 0x8000 0xC00 $partitionDump
  if ($LASTEXITCODE -ne 0) { throw "failed to read the current partition table" }
  $layout = (& $python $partitionTool $partitionDump | Out-String)
  if ($LASTEXITCODE -ne 0) { throw "failed to decode the current partition table" }
  if ($layout -match "(?m)^ota_0,") {
    throw "device already uses an A/B partition table; use Pet Manager OTA instead"
  }
  if ($layout -notmatch "(?m)^storage,.*0x210000,.*0x1000000") {
    throw "legacy 16MB SPIFFS partition at 0x210000 was not found; refusing migration"
  }

  Write-Host "Backing up the legacy SPIFFS partition to $BackupPath"
  & $esptool --chip esp32p4 --port $Port --before no-reset --after no-reset `
    read-flash 0x210000 0x1000000 $BackupPath
  if ($LASTEXITCODE -ne 0) { throw "SPIFFS backup failed" }
  if ((Get-Item -LiteralPath $BackupPath).Length -ne 0x1000000) {
    throw "SPIFFS backup has the wrong size; refusing to write the new partition table"
  }
  $backupHash = (Get-FileHash -LiteralPath $BackupPath -Algorithm SHA256).Hash
  Write-Host "Backup SHA-256: $backupHash"

  if (-not $FactoryReset) {
    throw "The current layout shrinks SPIFFS to make room for 640x480 custom video. Backup completed, but migration requires -FactoryReset."
  }

  Write-Host "Installing the current complete factory image; the legacy SPIFFS backup remains at $BackupPath"
  & (Join-Path $PSScriptRoot "p4.ps1") factory-flash -Port $Port -FactoryReset -PlatformIoCoreDir $coreDir
  if ($LASTEXITCODE -ne 0) {
    throw "Factory migration failed; keep the backup at $BackupPath for recovery"
  }

  Write-Host "Factory migration complete. Keep the backup until Pet Manager diagnostics pass."
} finally {
  Pop-Location
  Remove-Item -LiteralPath $partitionDump -Force -ErrorAction SilentlyContinue
}
