$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
$RefDir = Join-Path $RootDir "ref"
$ReleaseExe = Join-Path $RefDir "src-tauri\target\release\pet-manager-tauri.exe"

function Show-ExistingManager {
  param([string] $TargetPath)

  $existing = Get-Process -Name "pet-manager-tauri" -ErrorAction SilentlyContinue |
    Where-Object {
      try {
        [string]::Equals($_.Path, $TargetPath, [StringComparison]::OrdinalIgnoreCase)
      } catch {
        $false
      }
    } |
    Select-Object -First 1

  if (-not $existing) {
    return $false
  }

  if ($existing.MainWindowHandle -ne 0) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WindowTools {
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")]
  public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
}
"@
    [WindowTools]::ShowWindowAsync($existing.MainWindowHandle, 9) | Out-Null
    [WindowTools]::SetForegroundWindow($existing.MainWindowHandle) | Out-Null
  }

  Write-Host "HachimoDock Manager is already running. Reusing PID $($existing.Id)."
  return $true
}

if (Test-Path $ReleaseExe) {
  if (Show-ExistingManager -TargetPath $ReleaseExe) {
    exit 0
  }
  Start-Process -FilePath $ReleaseExe -WorkingDirectory (Split-Path -Parent $ReleaseExe)
  exit 0
}

Write-Host "HachimoDock Manager release executable was not found."
Write-Host "Building it now. The first run can take several minutes."
Write-Host ""

if (-not (Get-Command "npm.cmd" -ErrorAction SilentlyContinue)) {
  Write-Error "npm.cmd was not found. Please install Node.js, then run open-manager.bat again."
  exit 1
}

if (-not (Test-Path $RefDir)) {
  Write-Error "Could not find ref directory: $RefDir"
  exit 1
}

Set-Location $RefDir

if (-not (Test-Path (Join-Path $RefDir "node_modules"))) {
  Write-Host "Installing desktop dependencies..."
  & npm.cmd install
  if ($LASTEXITCODE -ne 0) {
    Write-Error "npm install failed."
    exit $LASTEXITCODE
  }
}

Write-Host "Building standalone desktop app..."
& npm.cmd run build
if ($LASTEXITCODE -ne 0) {
  Write-Error "Build failed. Make sure Rust, the Tauri prerequisites, and Node.js are installed."
  exit $LASTEXITCODE
}

if (-not (Test-Path $ReleaseExe)) {
  Write-Error "Build finished, but $ReleaseExe was not created."
  exit 1
}

Start-Process -FilePath $ReleaseExe -WorkingDirectory (Split-Path -Parent $ReleaseExe)
