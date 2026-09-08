param(
    [Parameter(Mandatory)][string]$BundlePath,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$iexpress = Join-Path $env:WINDIR 'System32\iexpress.exe'
if (-not (Test-Path -LiteralPath $iexpress -PathType Leaf)) {
    throw 'IExpress is not available on this Windows installation.'
}

$bundle = (Get-Item -LiteralPath $BundlePath -ErrorAction Stop).FullName
if (-not (Get-Item -LiteralPath $bundle).PSIsContainer) {
    throw 'BundlePath must be the prepared paired demo folder.'
}
if (-not $OutputPath) {
    $OutputPath = Join-Path $bundle 'raPId-AC1-Demo-Setup.exe'
}
$output = [IO.Path]::GetFullPath($OutputPath)
$files = @(
    'rapid-telemetry-daemon.exe', 'daemon.conf', 'telemetry.key',
    'start-rapid-daemon.vbs', 'START-RAPID.cmd',
    'install-demo.ps1', 'INSTALL-RAPID.cmd',
    'START-HERE.txt', 'AC1_DEMO_GUIDE.md'
)
foreach ($name in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $bundle $name) -PathType Leaf)) {
        throw "Bundle is missing required installer file: $name"
    }
}

$stage = Join-Path $env:TEMP ('rapid-demo-installer-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    foreach ($name in $files) {
        Copy-Item -LiteralPath (Join-Path $bundle $name) -Destination (Join-Path $stage $name)
    }
    $strings = for ($index = 0; $index -lt $files.Count; ++$index) {
        "FILE$index=$($files[$index])"
    }
    $sourceEntries = for ($index = 0; $index -lt $files.Count; ++$index) {
        "%FILE$index%="
    }
    $sed = @(
        '[Version]'
        'Class=IEXPRESS'
        'SEDVersion=3'
        '[Options]'
        'PackagePurpose=InstallApp'
        'ShowInstallProgramWindow=0'
        'HideExtractAnimation=1'
        'UseLongFileName=1'
        'InsideCompressed=1'
        'CAB_FixedSize=0'
        'CAB_ResvCodeSigning=0'
        'RebootMode=N'
        'InstallPrompt='
        'DisplayLicense='
        'FinishMessage='
        "TargetName=$output"
        'FriendlyName=raPId AC1 Demo Setup'
        'AppLaunched=cmd.exe /c INSTALL-RAPID.cmd'
        'PostInstallCmd=<None>'
        'AdminQuietInstCmd='
        'UserQuietInstCmd='
        'SourceFiles=SourceFiles'
        '[Strings]'
    ) + $strings + @(
        '[SourceFiles]'
        "SourceFiles0=$stage\"
        '[SourceFiles0]'
    ) + $sourceEntries
    $sedPath = Join-Path $stage 'installer.sed'
    [IO.File]::WriteAllLines($sedPath, $sed, [Text.UTF8Encoding]::new($false))
    $result = Start-Process -FilePath $iexpress -ArgumentList @('/N', $sedPath) -Wait -PassThru -WindowStyle Hidden
    if ($result.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "IExpress failed with exit code $($result.ExitCode)."
    }
    Write-Host "Created installer: $output"
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
