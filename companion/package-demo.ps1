param(
    [Parameter(Mandatory)][string]$ExecutablePath,
    [Parameter(Mandatory)][string]$AuthKeyPath,
    [Parameter(Mandatory)][string]$Destination,
    [ValidatePattern('\A[A-Za-z0-9][A-Za-z0-9._:-]{0,252}\z')][string]$PiHost = '192.168.1.64',
    [ValidateRange(1, 65535)][int]$PiPort = 9001
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Package an already built native binary. Never generate or print a pairing key.
$executable = (Get-Item -LiteralPath $ExecutablePath -ErrorAction Stop).FullName
$keyFile = Get-Item -LiteralPath $AuthKeyPath -ErrorAction Stop
if ((Get-Item -LiteralPath $executable).PSIsContainer -or $keyFile.PSIsContainer) {
    throw 'ExecutablePath and AuthKeyPath must be files.'
}
if ($keyFile.Length -gt 65536) { throw 'The pairing key file is unexpectedly large.' }
$keyText = [IO.File]::ReadAllText($keyFile.FullName).Trim()
if ($keyText -notmatch '\A[0-9a-fA-F]{64}\z') {
    throw 'The pairing key file must contain exactly 64 hexadecimal characters.'
}

$bundle = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
$repository = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\', '/')
$guide = Join-Path $repository 'docs\AC1_DEMO_GUIDE.md'
foreach ($required in @('START-RAPID.cmd', 'start-rapid-daemon.vbs', 'INSTALL-RAPID.cmd', 'install-demo.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $required) -PathType Leaf)) {
        throw "Package launcher is missing: $required"
    }
}
if (-not (Test-Path -LiteralPath $guide -PathType Leaf)) {
    throw 'docs/AC1_DEMO_GUIDE.md must be present before packaging the demo.'
}
$comparison = [StringComparison]::OrdinalIgnoreCase
function Test-InDirectory([string]$Candidate, [string]$Directory) {
    return $Candidate.Equals($Directory, $comparison) -or $Candidate.StartsWith($Directory + [IO.Path]::DirectorySeparatorChar, $comparison)
}
if ($bundle.Length -le [IO.Path]::GetPathRoot($bundle).TrimEnd('\', '/').Length) {
    throw 'Destination must be a named package folder, not a drive root.'
}
if ((Test-InDirectory $bundle $repository) -or (Test-InDirectory $repository $bundle)) {
    throw 'Keep the private demo package outside the public repository and its parent folders.'
}
if ((Test-InDirectory $executable $bundle) -or (Test-InDirectory $keyFile.FullName $bundle)) {
    throw 'Package inputs must be outside Destination so an existing package can be preserved.'
}
$parentDirectory = Split-Path $bundle -Parent
if (-not (Test-Path -LiteralPath $parentDirectory -PathType Container)) {
    throw 'Create the intended Destination parent folder first.'
}
# Reparse points can redirect a path into the public repository or a source folder.
$ancestor = Get-Item -LiteralPath $parentDirectory -Force
while ($null -ne $ancestor) {
    if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Use a Destination whose parent folders are not junctions or symbolic links.'
    }
    $ancestor = $ancestor.Parent
}
if (Test-Path -LiteralPath $bundle) {
    $existing = Get-Item -LiteralPath $bundle -Force
    if (-not $existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'An existing Destination must be a regular folder.'
    }
    $backup = $bundle + '.' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '.old'
    Move-Item -LiteralPath $bundle -Destination $backup
    Write-Host "Preserved previous package: $backup"
}

New-Item -ItemType Directory -Path $bundle | Out-Null
New-Item -ItemType Directory -Path (Join-Path $bundle 'recordings') | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $bundle 'rapid-telemetry-daemon.exe')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'START-RAPID.cmd') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'start-rapid-daemon.vbs') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL-RAPID.cmd') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install-demo.ps1') -Destination $bundle
$encoding = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $bundle 'telemetry.key'), $keyText + [Environment]::NewLine, $encoding)
$configuration = @(
    '# Private demo configuration. Keep telemetry.key with this paired Pi.'
    "pi_host=$PiHost"
    "pi_port=$PiPort"
    'sample_rate=50'
    'protocol=v4'
    'auth_key_file=telemetry.key'
    'output_directory=recordings'
    'local_recording=true'
    'no_forward=false'
)
[IO.File]::WriteAllLines((Join-Path $bundle 'daemon.conf'), $configuration, $encoding)
Copy-Item -LiteralPath $guide -Destination $bundle
foreach ($reference in @('TELEMETRY_V4.md', 'OPERATIONS.md')) {
    Copy-Item -LiteralPath (Join-Path $repository ('docs\' + $reference)) -Destination $bundle
}
$instructions = @(
    'raPId paired Assetto Corsa demo companion'
    ''
    '1. Double-click INSTALL-RAPID.cmd to install for this Windows user and start the companion.'
    '2. Connect the PC to Wi-Fi rapid. The Pi is 192.168.1.64 in AP mode.'
    '3. Launch Assetto Corsa through Steam or Content Manager.'
    '4. Enter a driving session and check live pedals, steering, graphs and recording on the Pi.'
    ''
    'Read AC1_DEMO_GUIDE.md before the demo for the full checklist and recovery steps.'
    'The installer creates a per-user raPId AC1 Demo folder and login startup entry.'
    'PC logs and emergency recordings are in recordings. Exit through the tray menu to finish recordings.'
    'telemetry.key is private and must match the Pi. Do not publish or upload this folder.'
)
[IO.File]::WriteAllLines((Join-Path $bundle 'START-HERE.txt'), $instructions, $encoding)
$hash = (Get-FileHash -LiteralPath (Join-Path $bundle 'rapid-telemetry-daemon.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $bundle 'EXECUTABLE.sha256'), "$hash  rapid-telemetry-daemon.exe" + [Environment]::NewLine, $encoding)
Write-Host "Prepared private demo package: $bundle"
Write-Host 'Start with START-RAPID.cmd. Keep the entire folder private; it contains the Pi pairing key.'
