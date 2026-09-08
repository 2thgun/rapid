param(
    [Parameter(Mandatory)][string]$ExecutablePath,
    [string]$OutputPath = 'dist\raPId-Companion.msi'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$wix = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
if (-not (Test-Path -LiteralPath $wix -PathType Leaf)) { throw 'Install WiX first: dotnet tool install --global wix --version 4.0.6' }
$exe = (Get-Item -LiteralPath $ExecutablePath -ErrorAction Stop).FullName
$root = Split-Path $PSScriptRoot -Parent
$output = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path $output -Parent) -Force | Out-Null
& $wix build -arch x64 -d "CompanionExe=$exe" -d "CompanionLauncher=$(Join-Path $PSScriptRoot 'start-rapid-daemon.vbs')" -d "ConfigExample=$(Join-Path $PSScriptRoot 'daemon.conf.example')" -o $output (Join-Path $root 'installer\raPIdCompanion.wxs')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output -PathType Leaf)) { throw 'WiX failed to build the MSI.' }
Write-Host "Created MSI: $output"
