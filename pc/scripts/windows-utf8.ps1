<#
 [Input] Repository root plus optional PlatformIO core override.
 [Output] UTF-8 process environment plus short ASCII sibling tool/temp paths for Windows build scripts.
 [Pos] Shared Windows environment helper for desktop and ESP32-P4 tooling.
 [Sync] If this file changes, update scripts/.folder.md.
#>

function Test-HachimoAsciiPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return -not [regex]::IsMatch($Path, "[^\x00-\x7F]")
}

function Get-HachimoDefaultToolingRoot {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $resolvedRoot = [System.IO.Path]::GetFullPath($RepoRoot)
    $repoParent = Split-Path -Parent $resolvedRoot
    if (-not $repoParent) {
        throw "Could not resolve a writable parent directory for: $resolvedRoot"
    }
    $toolingRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $repoParent ".hachimo-tooling")
    )
    if (-not (Test-HachimoAsciiPath -Path $toolingRoot)) {
        throw "Default tooling path must contain ASCII characters only: $toolingRoot"
    }
    return $toolingRoot
}

function Initialize-HachimoUtf8Environment {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $utf8 = [System.Text.UTF8Encoding]::new($false)
    try {
        [Console]::InputEncoding = $utf8
        [Console]::OutputEncoding = $utf8
    } catch {
        # Redirected and windowless processes may not expose a writable console.
    }
    $global:OutputEncoding = $utf8

    $env:PYTHONUTF8 = "1"
    $env:PYTHONIOENCODING = "utf-8"
    $env:LANG = "zh_CN.UTF-8"
    $env:LC_ALL = "zh_CN.UTF-8"

    if ($env:OS -eq "Windows_NT" -and (Get-Command "chcp.com" -ErrorAction SilentlyContinue)) {
        & chcp.com 65001 | Out-Null
    }

    $resolvedRoot = [System.IO.Path]::GetFullPath($RepoRoot)
    if (-not (Test-HachimoAsciiPath -Path $resolvedRoot)) {
        throw "Repository path must contain ASCII characters only for ESP-IDF builds: $resolvedRoot"
    }

    $toolingRoot = Get-HachimoDefaultToolingRoot -RepoRoot $resolvedRoot
    $tempRoot = Join-Path $toolingRoot "tmp"
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $env:TEMP = $tempRoot
    $env:TMP = $tempRoot
}

function Resolve-HachimoPlatformIoCoreDir {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [string]$RequestedPath = ""
    )

    $candidate = $RequestedPath.Trim()
    if (-not $candidate) {
        $candidate = "$env:HACHIMO_PLATFORMIO_CORE_DIR".Trim()
    }
    if (-not $candidate) {
        $candidate = "$env:PLATFORMIO_CORE_DIR".Trim()
    }
    if (-not $candidate) {
        $candidate = Join-Path (Get-HachimoDefaultToolingRoot -RepoRoot $RepoRoot) "platformio"
    }

    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-HachimoAsciiPath -Path $resolved)) {
        throw "PlatformIO core path must contain ASCII characters only: $resolved"
    }

    # ESP-IDF contains deeply nested OpenThread fixtures. Keep the core root
    # short enough for Windows hosts where long paths are not enabled.
    if ($resolved.Length -gt 48) {
        throw "PlatformIO core path is too long for portable ESP-IDF extraction ($($resolved.Length) > 48): $resolved"
    }

    New-Item -ItemType Directory -Force -Path $resolved | Out-Null
    $env:HACHIMO_PLATFORMIO_CORE_DIR = $resolved
    $env:PLATFORMIO_CORE_DIR = $resolved
    return $resolved
}

function Get-HachimoPythonPath {
    $managedPython = ""
    if ("$env:HACHIMO_PLATFORMIO_CORE_DIR".Trim()) {
        $managedPython = Join-Path $env:HACHIMO_PLATFORMIO_CORE_DIR "penv\Scripts\python.exe"
    }
    if ($managedPython -and (Test-Path -LiteralPath $managedPython)) {
        return [System.IO.Path]::GetFullPath($managedPython)
    }

    $command = Get-Command "python.exe" -ErrorAction SilentlyContinue
    if (-not $command) {
        $command = Get-Command "python" -ErrorAction SilentlyContinue
    }
    if (-not $command) {
        throw "Python was not found on PATH."
    }

    $path = $command.Source
    if (-not (Test-HachimoAsciiPath -Path $path)) {
        throw "Python executable path must contain ASCII characters only: $path"
    }
    return $path
}

function Get-HachimoEsptoolPath {
    param([Parameter(Mandatory = $true)][string]$PlatformIoCoreDir)

    $path = Join-Path $PlatformIoCoreDir "penv\Scripts\esptool.exe"
    if (-not (Test-Path -LiteralPath $path)) {
        throw "esptool was not found under the ASCII PlatformIO core: $path"
    }
    return $path
}

function Set-HachimoUserToolEnvironment {
    param([Parameter(Mandatory = $true)][string]$PlatformIoCoreDir)

    if (-not (Test-HachimoAsciiPath -Path $PlatformIoCoreDir)) {
        throw "Refusing to persist a non-ASCII PlatformIO path: $PlatformIoCoreDir"
    }

    [Environment]::SetEnvironmentVariable(
        "HACHIMO_PLATFORMIO_CORE_DIR",
        $PlatformIoCoreDir,
        "User"
    )
    [Environment]::SetEnvironmentVariable(
        "PLATFORMIO_CORE_DIR",
        $PlatformIoCoreDir,
        "User"
    )
    [Environment]::SetEnvironmentVariable("PYTHONUTF8", "1", "User")
    [Environment]::SetEnvironmentVariable("PYTHONIOENCODING", "utf-8", "User")
}
