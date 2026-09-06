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

# One-time migration: retire only a legacy PowerShell process whose command line
# names this daemon. Failure to inspect processes is non-fatal; the named mutex
# still prevents duplicate recorders.
try {
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'pwsh.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine -match 'rapid-telemetry-daemon\.ps1' } |
        ForEach-Object { [void](Invoke-CimMethod -InputObject $_ -MethodName Terminate) }
} catch {
    Write-Warning "Could not retire the legacy daemon automatically; use its tray Exit command once: $($_.Exception.Message)"
}

if (-not $NoAutoStart) {
    $command = 'wscript.exe "{0}"' -f $launcher
    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name $name -Value $command
    Write-Host 'raPId telemetry daemon will start automatically at login.'
}
Start-Process wscript.exe -ArgumentList ('"{0}"' -f $launcher)
Write-Host 'Native raPId telemetry daemon started. It remains dormant until a supported simulator EXE runs.'
