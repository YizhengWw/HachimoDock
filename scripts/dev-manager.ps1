<#
 [Input] Optional Vite port for a Windows Pet Manager development session.
 [Output] Foreground Tauri dev process with UTF-8 console, logs, and child environment.
 [Pos] Canonical Windows development launcher.
 [Sync] If this file changes, update scripts/.folder.md.
#>
param(
    [ValidateRange(1024, 65535)]
    [int]$Port = 4173
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$refRoot = Join-Path $repoRoot "ref"
$environmentScript = Join-Path $PSScriptRoot "windows-utf8.ps1"

. $environmentScript
Initialize-HachimoUtf8Environment -RepoRoot $repoRoot

$npm = Get-Command "npm.cmd" -ErrorAction SilentlyContinue
if (-not $npm) {
    throw "npm.cmd was not found on PATH."
}
if (-not (Test-HachimoAsciiPath -Path $npm.Source)) {
    throw "Node/npm path must contain ASCII characters only: $($npm.Source)"
}

$arguments = @("run", "dev")
if ($Port -ne 4173) {
    $logRoot = Join-Path $repoRoot ".dev-logs"
    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    $configPath = Join-Path $logRoot "tauri-dev-$Port.json"
    $config = @{
        build = @{
            beforeDevCommand = "npm run dev:web -- --host 127.0.0.1 --port $Port --strictPort"
            devUrl = "http://127.0.0.1:$Port"
        }
    } | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText(
        $configPath,
        $config,
        [System.Text.UTF8Encoding]::new($false)
    )
    $arguments += @("--", "--config", $configPath)
}

Push-Location $refRoot
try {
    & $npm.Source @arguments
    exit $LASTEXITCODE
} finally {
    Pop-Location
}