<#
.SYNOPSIS
Builds the telaviva_core and telaviva_inference cdylibs and copies the DLLs,
import libraries and PDBs into an output directory for the Windows shell.

.PARAMETER Configuration
Release (default) or Debug.

.PARAMETER OutDir
Destination directory. Relative paths resolve from the repository root.
Defaults to native\windows\build\rust.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

# This script lives in tools\windows, so the repository root is two levels up.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path $repoRoot 'native\windows\build\rust'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

# Rustup installs cargo here but the current shell may not have it on PATH.
$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
if (-not (Get-Command cargo.exe -ErrorAction SilentlyContinue)) {
    $env:PATH = $cargoBin + ';' + $env:PATH
}
if (-not (Get-Command cargo.exe -ErrorAction SilentlyContinue)) {
    Fail "cargo.exe not found on PATH or in $cargoBin. Install Rust with rustup first."
}

$profileDir = 'debug'
$profileArgs = @()
if ($Configuration -eq 'Release') {
    $profileDir = 'release'
    $profileArgs = @('--release')
}

# Respect a caller-provided target directory; otherwise use cargo's default.
if (-not [string]::IsNullOrEmpty($env:CARGO_TARGET_DIR)) {
    $targetRoot = $env:CARGO_TARGET_DIR
    if (-not [System.IO.Path]::IsPathRooted($targetRoot)) {
        $targetRoot = Join-Path $repoRoot $targetRoot
    }
}
else {
    $targetRoot = Join-Path $repoRoot 'target'
}
$profileTarget = Join-Path $targetRoot $profileDir

Push-Location -LiteralPath $repoRoot
try {
    foreach ($pkg in @('telaviva-core', 'telaviva-inference')) {
        # The inference DLL carries DirectML registration so it can drive either runtime.
        $featureArgs = @()
        if ($pkg -eq 'telaviva-inference') {
            $featureArgs = @('--features', 'directml')
        }
        $cargoArgs = @('rustc', '--lib', '--crate-type', 'cdylib', '-p', $pkg, '--locked') + $featureArgs + $profileArgs
        Write-Host "> cargo $($cargoArgs -join ' ')"
        & cargo.exe @cargoArgs
        # $? is unreliable for native commands; the exit code is authoritative.
        if ($LASTEXITCODE -ne 0) {
            Fail "cargo rustc failed for $pkg with exit code $LASTEXITCODE."
        }
    }
}
finally {
    Pop-Location
}

$requiredFiles = @(
    'telaviva_core.dll',
    'telaviva_core.dll.lib',
    'telaviva_inference.dll',
    'telaviva_inference.dll.lib'
)
$optionalFiles = @(
    'telaviva_core.pdb',
    'telaviva_inference.pdb'
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$copied = @()
foreach ($name in $requiredFiles) {
    $src = Join-Path $profileTarget $name
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        Fail "Expected build output is missing: $src"
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $OutDir $name) -Force
    $copied += $name
}

foreach ($name in $optionalFiles) {
    $src = Join-Path $profileTarget $name
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $OutDir $name) -Force
        $copied += $name
    }
}

# The native linker looks for plain .lib names as well as the cargo-style .dll.lib.
foreach ($crate in @('telaviva_core', 'telaviva_inference')) {
    $libName = "$crate.dll.lib"
    $aliasName = "$crate.lib"
    Copy-Item -LiteralPath (Join-Path $OutDir $libName) -Destination (Join-Path $OutDir $aliasName) -Force
    $copied += $aliasName
}

Write-Host ''
Write-Host "Output directory: $OutDir"
Get-ChildItem -LiteralPath $OutDir -File |
    Where-Object { $copied -contains $_.Name } |
    Sort-Object Name |
    ForEach-Object { '{0,-30} {1,12:N0} bytes' -f $_.Name, $_.Length } |
    Write-Host
