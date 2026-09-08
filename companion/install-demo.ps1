param(
    [switch]$NoAutoStart,
    [switch]$Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = (Get-Item -LiteralPath $PSScriptRoot -Force).FullName
$destination = Join-Path $env:LOCALAPPDATA 'raPId\AC1 Demo'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'raPId AC1 Demo'

if ($Remove) {
    Remove-ItemProperty -Path $runKey -Name $runName -ErrorAction SilentlyContinue
    Write-Host 'Removed raPId AC1 Demo from Windows login startup. Installed files and recordings remain in:'
    Write-Host $destination
    exit 0
}

foreach ($name in @('rapid-telemetry-daemon.exe', 'daemon.conf', 'telemetry.key', 'start-rapid-daemon.vbs')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) {
        throw "Demo package is incomplete: $name is missing."
    }
}

$sourceFull = [IO.Path]::GetFullPath($source).TrimEnd('\')
$destinationFull = [IO.Path]::GetFullPath($destination).TrimEnd('\')
if (-not $sourceFull.Equals($destinationFull, [StringComparison]::OrdinalIgnoreCase)) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $source -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destination $_.Name) -Force
    }
}
New-Item -ItemType Directory -Path (Join-Path $destination 'recordings') -Force | Out-Null

if (-not $NoAutoStart) {
    New-Item -Path $runKey -Force | Out-Null
    $launcher = Join-Path $destination 'start-rapid-daemon.vbs'
    Set-ItemProperty -Path $runKey -Name $runName -Value ('wscript.exe "{0}"' -f $launcher)
}

$installedLauncher = Join-Path $destination 'start-rapid-daemon.vbs'
Start-Process -FilePath 'wscript.exe' -ArgumentList ('"{0}"' -f $installedLauncher)
Write-Host "Installed raPId AC1 Demo in: $destination"
if ($NoAutoStart) {
    Write-Host 'The companion is running for this session only.'
} else {
    Write-Host 'The companion will also start at Windows login.'
}
