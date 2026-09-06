param(
    [switch]$Clean,
    [ValidateSet('Auto', 'Zig', 'MSVC')][string]$Compiler = 'Auto',
    [string]$ZigPath = '',
    [string]$OutputDirectory = $PSScriptRoot,
    [string]$CacheDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'rapid-zig-cache')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'native\rapid-telemetry-daemon.cpp'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$output = Join-Path $OutputDirectory 'rapid-telemetry-daemon.exe'

if ($Clean) {
    Remove-Item -LiteralPath $output -ErrorAction SilentlyContinue
    return
}

if (-not (Test-Path -LiteralPath $source)) { throw "Native source not found: $source" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$commonLibraries = @('-lws2_32', '-lshell32', '-lole32', '-luuid', '-lbcrypt')
$clang = Get-Command clang++ -ErrorAction SilentlyContinue
$gcc = Get-Command g++ -ErrorAction SilentlyContinue
$zig = Get-Command zig -ErrorAction SilentlyContinue
$portableZig = Join-Path (Split-Path $PSScriptRoot -Parent) '.tools\zig\zig.exe'
if ($ZigPath) { $zig = Get-Item -LiteralPath $ZigPath -ErrorAction Stop }
if (-not $zig -and (Test-Path -LiteralPath $portableZig)) {
    $zig = Get-Item -LiteralPath $portableZig
}
$built = $false
if ($Compiler -eq 'Zig') {
    $clang = $null
    $gcc = $null
    if (-not $zig) { throw 'Supply -ZigPath or install Zig on PATH.' }
} elseif ($Compiler -eq 'MSVC') {
    $clang = $null
    $gcc = $null
    $zig = $null
}

if ($clang) {
    & $clang.Source -std=c++20 -O2 -DNDEBUG -municode -static-libgcc -static-libstdc++ `
        $source -o $output @commonLibraries
    $built = $LASTEXITCODE -eq 0
} elseif ($gcc) {
    & $gcc.Source -std=c++20 -O2 -DNDEBUG -municode -static -s `
        $source -o $output @commonLibraries
    $built = $LASTEXITCODE -eq 0
} elseif ($zig) {
    $zigCommand = if ($zig -is [Management.Automation.CommandInfo]) { $zig.Source } else { $zig.FullName }
    $zigCacheRoot = [IO.Path]::GetFullPath($CacheDirectory)
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $zigCacheRoot 'global'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $zigCacheRoot 'local'
    & $zigCommand c++ -target x86_64-windows-gnu -std=c++20 -O2 -DNDEBUG -municode `
        $source -o $output @commonLibraries
    $built = $LASTEXITCODE -eq 0
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) {
            $devShell = Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
            Import-Module $devShell
            Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64'
            $objectOutput = Join-Path $OutputDirectory 'rapid-telemetry-daemon.obj'
            & cl.exe /nologo /std:c++20 /O2 /DNDEBUG /EHsc /W4 /DUNICODE /D_UNICODE `
                $source /Fe:$output /Fo:$objectOutput /link ws2_32.lib shell32.lib ole32.lib uuid.lib bcrypt.lib
            $built = $LASTEXITCODE -eq 0
        }
    }
}

if (-not $built -or -not (Test-Path -LiteralPath $output)) {
    throw 'No supported C++ toolchain was found, or the native build failed. Install LLVM, MinGW-w64, Zig, or Visual Studio Build Tools and retry.'
}

Write-Host "Built native telemetry daemon: $output"
