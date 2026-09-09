param(
    [switch]$NoAutoStart,
    [switch]$Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$name = 'raPId Telemetry Daemon'
$launcher = Join-Path $PSScriptRoot 'start-rapid-daemon.vbs'

if ($Remove) {
    Remove-ItemProperty -Path $runKey -Name $name -ErrorAction SilentlyContinue
    Write-Host 'Removed raPId telemetry daemon from Windows login startup.'
    return
}

if (-not (Test-Path $launcher)) { throw "Launcher not found: $launcher" }
$native = Join-Path $PSScriptRoot 'rapid-telemetry-daemon.exe'
if (-not (Test-Path -LiteralPath $native)) {
    throw "Native daemon not found: $native. Run .\build-native-daemon.ps1 first."
}

if (-not $NoAutoStart) {
    $command = 'wscript.exe "{0}"' -f $launcher
    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name $name -Value $command
    Write-Host 'raPId telemetry daemon will start automatically at login.'
}
Start-Process wscript.exe -ArgumentList ('"{0}"' -f $launcher)
Write-Host 'Native raPId telemetry daemon started. It remains dormant until a supported simulator EXE runs.'
