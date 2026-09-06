<#
Compatibility entry point. The ACC-only recorder was replaced by the unified
raPId tray daemon; existing shortcuts and commands continue to work.
#>
param(
    [string]$PiHost = '255.255.255.255',
    [int]$PiPort = 9001,
    [int]$SampleRate = 10,
    [string]$OutputDirectory = "$env:USERPROFILE\Documents\raPId Telemetry",
    [switch]$NoForward,
    [switch]$Headless,
    [switch]$SelfTest
)

$native = Join-Path $PSScriptRoot 'rapid-telemetry-daemon.exe'
if (Test-Path -LiteralPath $native) {
    $arguments = @('--pi-host', $PiHost, '--pi-port', $PiPort, '--sample-rate', $SampleRate,
                   '--output-directory', $OutputDirectory)
    if ($NoForward) { $arguments += '--no-forward' }
    if ($Headless) { $arguments += '--headless' }
    if ($SelfTest) { $arguments += '--self-test' }
    & $native @arguments
} else {
    $daemon = Join-Path $PSScriptRoot 'rapid-telemetry-daemon.ps1'
    & $daemon @PSBoundParameters
}
