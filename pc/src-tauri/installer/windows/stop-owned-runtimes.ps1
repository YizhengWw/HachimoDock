<#
 [Input] Pet Manager installation directory supplied by the Windows installer.
 [Output] Stops only Pet Manager executables rooted in that exact directory and
          fails when an owned release file remains locked.
 [Pos] Shared pre-install/pre-uninstall runtime cleanup for Windows packages.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    } catch {
        return ""
    }
}

function Get-ProcessesAtPath {
    param([Parameter(Mandatory = $true)][string]$ExpectedPath)

    $normalizedExpected = Resolve-NormalizedPath -Path $ExpectedPath
    if (-not $normalizedExpected) {
        return @()
    }

    return @(
        Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
            $_.ExecutablePath -and
            (Resolve-NormalizedPath -Path $_.ExecutablePath).Equals(
                $normalizedExpected,
                [System.StringComparison]::OrdinalIgnoreCase
            )
        }
    )
}

function Stop-ProcessesAtPath {
    param([Parameter(Mandatory = $true)][string]$ExpectedPath)

    foreach ($process in Get-ProcessesAtPath -ExpectedPath $ExpectedPath) {
        & "$env:SystemRoot\System32\taskkill.exe" /PID $process.ProcessId /T /F | Out-Null
    }
}

function Test-ExclusiveWriteAccess {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $true
    }

    try {
        $stream = [System.IO.File]::Open(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None
        )
        $stream.Dispose()
        return $true
    } catch {
        return $false
    }
}

$normalizedInstallDir = Resolve-NormalizedPath -Path $InstallDir
if (-not $normalizedInstallDir) {
    throw "Invalid Pet Manager installation directory."
}

$ownedPaths = @(
    (Join-Path $normalizedInstallDir "pet-manager-tauri.exe"),
    (Join-Path $normalizedInstallDir "Pet Manager.exe"),
    (Join-Path $normalizedInstallDir "bridge\runtime\node.exe")
)

# Stop the desktop first so it cannot restart the bridge while files are replaced.
foreach ($ownedPath in $ownedPaths) {
    Stop-ProcessesAtPath -ExpectedPath $ownedPath
}

$deadline = [DateTime]::UtcNow.AddSeconds(8)
do {
    $liveOwnedProcesses = @(
        foreach ($ownedPath in $ownedPaths) {
            Get-ProcessesAtPath -ExpectedPath $ownedPath
        }
    )
    $lockedPaths = @($ownedPaths | Where-Object { -not (Test-ExclusiveWriteAccess -Path $_) })
    if ($liveOwnedProcesses.Count -eq 0 -and $lockedPaths.Count -eq 0) {
        exit 0
    }
    Start-Sleep -Milliseconds 150
} while ([DateTime]::UtcNow -lt $deadline)

$remaining = @(
    $ownedPaths | Where-Object {
        (Get-ProcessesAtPath -ExpectedPath $_).Count -gt 0 -or
        -not (Test-ExclusiveWriteAccess -Path $_)
    }
)
throw "Pet Manager files remain in use: $($remaining -join ', ')"
