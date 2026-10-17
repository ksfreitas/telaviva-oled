<#
.SYNOPSIS
Builds the Windows 11 x64 app and the release zip in one command.

.DESCRIPTION
Build the Windows 11 x64 app into dist\TelaVivaOLED and the release zip dist\TelaVivaOLED-<version>-win-x64.zip. Run from any directory. The first run downloads the pinned ONNX Runtime.

Steps: check the prerequisites, fetch the pinned ONNX Runtime when it is missing, run packaging\windows\build-app.ps1, then run native\windows\build\x64\<Configuration>\tests.exe. The dist folder and zip are kept when the tests fail.

Run with Windows PowerShell 5.1:
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1

.PARAMETER Configuration
Release (default) or Debug.

.PARAMETER SkipTests
Do not run tests.exe after the build.

.PARAMETER NoZip
Write the dist folder only, without the release zip.

.PARAMETER Help
Print the usage and exit 0.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -NoZip
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',

    [switch]$SkipTests,

    [switch]$NoZip,

    [switch]$Help,

    # Collects unknown arguments so they can be reported with the usage text, exit code 2.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$UnknownArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-UsageText {
    return @'
Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 [-Configuration Release|Debug] [-SkipTests] [-NoZip] [-Help]

Build the Windows 11 x64 app into dist\TelaVivaOLED and the release zip dist\TelaVivaOLED-<version>-win-x64.zip.
Run from any directory. The first run downloads the pinned ONNX Runtime.

Options:
  -Configuration Release|Debug  Build configuration (default Release).
  -SkipTests                    Do not run tests.exe after the build.
  -NoZip                        Write the dist folder only, without the release zip.
  -Help                         Print this usage and exit.
'@
}

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("build-windows: ${Message}")
    exit 1
}

function Find-VsInstallation {
    param([string]$VsWhere, [string]$ComponentId)
    $found = @(& $VsWhere -latest -products '*' -requires $ComponentId -property installationPath)
    if ($LASTEXITCODE -ne 0) { Fail "vswhere failed with exit code ${LASTEXITCODE}." }
    return ($found | Where-Object { -not [string]::IsNullOrEmpty($_) } | Select-Object -First 1)
}

if ($Help) {
    [Console]::Out.WriteLine((Get-UsageText))
    exit 0
}

if ($UnknownArguments.Count -gt 0) {
    [Console]::Error.WriteLine("build-windows: Unknown argument: $($UnknownArguments -join ' ')")
    [Console]::Error.WriteLine((Get-UsageText))
    exit 2
}

# Configuration is checked by hand so a bad value gets the usage text and exit code 2, like build-macos.sh.
if ($Configuration -ieq 'Release') {
    $Configuration = 'Release'
}
elseif ($Configuration -ieq 'Debug') {
    $Configuration = 'Debug'
}
else {
    [Console]::Error.WriteLine("build-windows: -Configuration must be Release or Debug, not '${Configuration}'.")
    [Console]::Error.WriteLine((Get-UsageText))
    exit 2
}

# This script lives in scripts, so the repository root is one level up.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) { Fail 'This build requires Windows.' }
if (-not [System.Environment]::Is64BitOperatingSystem) { Fail 'This build requires a 64-bit Windows installation.' }

# rustup adds cargo to the user PATH, but a session opened before that install does not see it.
$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
if ((-not (Get-Command cargo -ErrorAction SilentlyContinue)) -and (Test-Path -LiteralPath (Join-Path $cargoBin 'cargo.exe') -PathType Leaf)) {
    $env:Path = "${cargoBin};${env:Path}"
}
foreach ($tool in @('cargo', 'rustc')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "Missing ${tool}. Install Rust with rustup (stable, x86_64-pc-windows-msvc) and make sure %USERPROFILE%\.cargo\bin is on PATH."
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    Fail "vswhere.exe not found at ${vswhere}. Install Visual Studio 2022 with the C++ tools."
}
$vcToolsId = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$vsPath = Find-VsInstallation -VsWhere $vswhere -ComponentId $vcToolsId
if ([string]::IsNullOrEmpty($vsPath)) {
    Fail "No Visual Studio installation with the C++ tools (${vcToolsId}) was found. Install the Desktop development with C++ workload."
}
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) { Fail "MSBuild not found: ${msbuild}." }

# The Visual Studio Installer is only named in the hint. This script never installs components.
$setupExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
$requiredComponents = @(
    'Microsoft.Component.MSBuild',
    'Microsoft.VisualStudio.Component.Roslyn.Compiler',
    'Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp'
)
foreach ($componentId in $requiredComponents) {
    $componentPath = Find-VsInstallation -VsWhere $vswhere -ComponentId $componentId
    if ([string]::IsNullOrEmpty($componentPath)) {
        Fail "Missing Visual Studio component ${componentId}. Add it in the Visual Studio Installer, or from an elevated prompt: `"${setupExe}`" modify --installPath `"${vsPath}`" --add ${componentId} --passive --norestart"
    }
}

$sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\10.0.26100.0'
if (-not (Test-Path -LiteralPath $sdkInclude -PathType Container)) {
    Fail "Windows SDK 10.0.26100.0 not found at ${sdkInclude}. Add the Windows 11 SDK (10.0.26100) in the Visual Studio Installer."
}

# The release version comes from the core crate, the same source build-app.ps1 uses.
$versionManifest = Join-Path $repoRoot 'crates\telaviva-core\Cargo.toml'
if (-not (Test-Path -LiteralPath $versionManifest -PathType Leaf)) { Fail "Version manifest is missing: ${versionManifest}." }
$version = ''
foreach ($line in Get-Content -LiteralPath $versionManifest) {
    if ($line -match '^version\s*=\s*"([^"]+)"') {
        $version = $Matches[1]
        break
    }
}
if ([string]::IsNullOrEmpty($version)) { Fail "No version key found in ${versionManifest}." }

$distDir = Join-Path $repoRoot 'dist\TelaVivaOLED'
$zipPath = Join-Path $repoRoot "dist\TelaVivaOLED-${version}-win-x64.zip"
$testsExe = Join-Path $repoRoot "native\windows\build\x64\${Configuration}\tests.exe"

# Fetch: the pin check lives in the fetch script, so a failed fetch stops the build here.
$runtimeDll = Join-Path $repoRoot 'tools\inference\runtime\windows-x64\onnxruntime.dll'
if (-not (Test-Path -LiteralPath $runtimeDll -PathType Leaf)) {
    $fetchScript = Join-Path $repoRoot 'tools\inference\fetch-windows-runtime.ps1'
    if (-not (Test-Path -LiteralPath $fetchScript -PathType Leaf)) { Fail "Fetch script is missing: ${fetchScript}." }
    Write-Host '> fetch-windows-runtime.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fetchScript
    if ($LASTEXITCODE -ne 0) { Fail "fetch-windows-runtime.ps1 failed with exit code ${LASTEXITCODE}. The build was not started." }
}

# Build: a child process keeps build-app.ps1's exit code separate from this script's.
$buildScript = Join-Path $repoRoot 'packaging\windows\build-app.ps1'
if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) { Fail "Build script is missing: ${buildScript}." }
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Configuration', $Configuration)
$buildNote = ''
if ($NoZip) {
    $buildArgs += '-Zip:$false'
    $buildNote = ' -Zip:$false'
}
Write-Host "> build-app.ps1 -Configuration ${Configuration}${buildNote}"
& powershell.exe @buildArgs
if ($LASTEXITCODE -ne 0) { Fail "build-app.ps1 failed with exit code ${LASTEXITCODE}." }

if (-not $NoZip -and -not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    Fail "build-app.ps1 finished but the zip is missing: ${zipPath}."
}

# Tests run after build-app.ps1, because tests.exe is produced by its MSBuild step.
$produced = "the dist folder ${distDir}"
if (-not $NoZip) { $produced = "${produced} and the zip ${zipPath}" }
if ($SkipTests) {
    Write-Host 'Tests skipped (-SkipTests).'
}
else {
    if (-not (Test-Path -LiteralPath $testsExe -PathType Leaf)) {
        Fail "tests.exe was not produced at ${testsExe}. ${produced} was produced; nothing was removed."
    }
    Write-Host "> tests.exe ($Configuration)"
    $testLines = @(& $testsExe | ForEach-Object { Write-Host $_; $_ })
    $testsExitCode = $LASTEXITCODE
    $lastLine = ''
    if ($testLines.Count -gt 0) { $lastLine = [string]$testLines[-1] }
    Write-Host "Tests: ${lastLine}"
    if ($testsExitCode -ne 0) {
        Fail "tests.exe failed with exit code ${testsExitCode}. ${produced} was produced, and nothing was removed. The build output is still usable for inspection; fix the tests before release."
    }
}

Write-Host ''
Write-Host "Folder:  ${distDir}"
if (-not $NoZip) {
    $zipItem = Get-Item -LiteralPath $zipPath
    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    Write-Host "Zip:     ${zipPath}"
    Write-Host ('Size:    {0:N1} MB ({1:N0} bytes)' -f ($zipItem.Length / 1MB), $zipItem.Length)
    Write-Host "SHA-256: ${zipHash}"
}
Write-Host 'Done.'
exit 0
