<#
.SYNOPSIS
    Fetches and verifies the ONNX Runtime 1.20.1 build for Windows x64.

.DESCRIPTION
    Purpose: recreates the ONNX Runtime 1.20.1 folders from Microsoft's official
    releases, as fetch-macos-runtime.sh does for macOS. The Rust crate loads the
    DLL dynamically (ort load-dynamic).

    -Variant cpu (default): tools/inference/runtime/windows-x64/ (onnxruntime.dll,
    onnxruntime_providers_shared.dll when the archive has it, and the license
    files) from onnxruntime-win-x64-1.20.1.zip on the GitHub release. The Rust CPU
    smoke tests read this folder.
    -Variant directml: tools/inference/runtime/windows-x64-directml/ from the
    Microsoft.ML.OnnxRuntime.DirectML NuGet package (same ONNX Runtime version as
    the CPU pin). This is the only runtime that build-app.ps1 ships, as
    onnxruntime.dll (specs/archived/inference-device-selection, architecture Decision 2).
    DirectML.dll is never extracted (the in-box System32 copy is used). Requires
    the [runtime.windows-x64.directml] pins.

    -ComputeOnly: downloads into a new temporary folder, prints the SHA-256 values
    and archive size for the manifest pins, and exits without touching the repo.
    Works for both variants.

    -Force: reinstalls even when the DLL already matches its pin.

    Safety rules:
    - Downloads are untrusted data. Each one goes into a new, empty folder under
      $env:TEMP, and all such folders are removed in a finally block.
    - Pins come from assets/model/manifest.toml. Nothing is copied into the repo
      unless the archive and the extracted DLL match the pinned SHA-256 values.
    - Only the named DLLs and license files are extracted. DirectML.dll is never
      extracted (the in-box System32 copy is used).
    - Nothing downloaded is executed or imported.

    Run from any directory with Windows PowerShell 5.1:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\inference\fetch-windows-runtime.ps1
#>
param(
    [ValidateSet('cpu', 'directml')]
    [string]$Variant = 'cpu',
    [switch]$ComputeOnly,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

$script:TempDirs = @()
$exitCode = 1

# Paths are resolved here, not in parameter defaults.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$manifestPath = Join-Path $repoRoot 'assets\model\manifest.toml'
$runtimeDir = Join-Path $scriptDir 'runtime\windows-x64'
if ($Variant -eq 'directml') {
    $runtimeDir = Join-Path $scriptDir 'runtime\windows-x64-directml'
}

$cpuUrl = 'https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip'
$directMlUrl = 'https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1'
# Exact entry names of the DirectML package. Nothing else is ever extracted.
$directMlDllEntry = 'runtimes/win-x64/native/onnxruntime.dll'
$directMlLicenseEntries = @('LICENSE', 'LICENSE.txt', 'ThirdPartyNotices.txt')
$directMlRequiredLicenses = @('LICENSE', 'ThirdPartyNotices.txt')

function New-EmptyTempDir {
    $path = Join-Path $env:TEMP ('telaviva-ort-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    $script:TempDirs += $path
    return $path
}

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Save-Download {
    param([string]$Url, [string]$Destination)
    Write-Host "Downloading $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Destination -TimeoutSec 300
}

function Read-ManifestSection {
    param([string]$Section)
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Manifest not found: $manifestPath"
    }
    $values = @{}
    $inSection = $false
    foreach ($line in Get-Content -LiteralPath $manifestPath) {
        if ($line -match '^\s*\[([^\[\]]+)\]\s*(#.*)?$') {
            $inSection = ($matches[1].Trim() -eq $Section)
        }
        elseif ($inSection -and $line -match '^\s*([A-Za-z0-9_-]+)\s*=\s*"([^"]*)"\s*(#.*)?$') {
            $values[$matches[1]] = $matches[2]
        }
    }
    return $values
}

function Test-Sha256Pin {
    param([string]$Value)
    return [bool]($Value -match '^[0-9a-fA-F]{64}$')
}

function Get-ManifestPin {
    param([hashtable]$Values, [string]$Section, [string]$Key)
    $value = ''
    if ($Values.ContainsKey($Key)) { $value = [string]$Values[$Key] }
    if (-not (Test-Sha256Pin $value)) {
        throw "Manifest [$Section] $Key is missing, empty or still a placeholder. Run this script with -ComputeOnly and record the pins in assets/model/manifest.toml first."
    }
    return $value.ToLowerInvariant()
}

function Get-EntriesByPattern {
    param($Zip, [string]$Pattern)
    return @($Zip.Entries | Where-Object { $_.Name -ne '' -and $_.FullName -match $Pattern })
}

function Get-SingleEntry {
    param($Entries, [string]$Description)
    $list = @($Entries)
    if ($list.Count -ne 1) {
        throw "Expected exactly one $Description in the archive, found $($list.Count). Nothing was installed."
    }
    return $list[0]
}

function Get-LicenseEntries {
    param($Zip)
    $found = @()
    foreach ($name in @('LICENSE', 'ThirdPartyNotices.txt', 'Privacy.md')) {
        $candidates = @($Zip.Entries | Where-Object { $_.Name -eq $name } | Sort-Object { ($_.FullName -split '/').Count })
        if ($candidates.Count -gt 0) {
            $found += $candidates[0]
        }
        elseif ($name -ne 'Privacy.md') {
            throw "Archive has no $name. Nothing was installed."
        }
    }
    return $found
}

function Expand-ToStaging {
    param($Entry, [string]$StagingDir)
    $dest = Join-Path $StagingDir $Entry.Name
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $dest, $true)
    return $dest
}

function Publish-Files {
    param([string[]]$Files)
    New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
    foreach ($file in $Files) {
        $dest = Join-Path $runtimeDir (Split-Path -Leaf $file)
        Copy-Item -LiteralPath $file -Destination $dest -Force
        Write-Host "Installed $dest"
    }
}

function Invoke-ComputeCpu {
    $work = New-EmptyTempDir
    $archive = Join-Path $work 'onnxruntime-win-x64-1.20.1.zip'
    Save-Download -Url $cpuUrl -Destination $archive
    $archiveHash = Get-Sha256 $archive
    $archiveSize = (Get-Item -LiteralPath $archive).Length

    $extract = New-EmptyTempDir
    Expand-Archive -LiteralPath $archive -DestinationPath $extract
    $dlls = @(Get-ChildItem -LiteralPath $extract -Recurse -File -Filter 'onnxruntime.dll' |
        Where-Object { (Split-Path -Leaf (Split-Path -Parent $_.FullName)) -eq 'lib' })
    if ($dlls.Count -ne 1) {
        throw "Expected exactly one lib\onnxruntime.dll in the archive, found $($dlls.Count)."
    }
    $dllHash = Get-Sha256 $dlls[0].FullName

    Write-Host "variant:        cpu (ONNX Runtime 1.20.1)"
    Write-Host "archive_sha256 = `"$archiveHash`""
    Write-Host "dll_sha256     = `"$dllHash`""
    Write-Host "archive size:   $archiveSize bytes"
}

function Install-Cpu {
    $section = 'runtime.windows-x64.cpu'
    $pins = Read-ManifestSection -Section $section
    $archivePin = Get-ManifestPin -Values $pins -Section $section -Key 'archive_sha256'
    $dllPin = Get-ManifestPin -Values $pins -Section $section -Key 'dll_sha256'

    $targetDll = Join-Path $runtimeDir 'onnxruntime.dll'
    if (-not $Force -and (Test-Path -LiteralPath $targetDll) -and ((Get-Sha256 $targetDll) -eq $dllPin)) {
        Write-Host "Verified onnxruntime.dll already present in $runtimeDir. Nothing to do (use -Force to refetch)."
        return
    }

    $work = New-EmptyTempDir
    $archive = Join-Path $work 'onnxruntime-win-x64-1.20.1.zip'
    Save-Download -Url $cpuUrl -Destination $archive
    $archiveHash = Get-Sha256 $archive
    if ($archiveHash -ne $archivePin) {
        throw "Archive SHA-256 mismatch: expected $archivePin, got $archiveHash. Nothing was installed."
    }

    $staging = New-EmptyTempDir
    $zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $dllEntry = Get-SingleEntry -Entries (Get-EntriesByPattern -Zip $zip -Pattern '(^|/)lib/onnxruntime\.dll$') -Description 'lib/onnxruntime.dll'
        $sharedEntries = @(Get-EntriesByPattern -Zip $zip -Pattern '(^|/)lib/onnxruntime_providers_shared\.dll$')
        $licenseEntries = @(Get-LicenseEntries -Zip $zip)

        $stagedDll = Expand-ToStaging -Entry $dllEntry -StagingDir $staging
        $dllHash = Get-Sha256 $stagedDll
        if ($dllHash -ne $dllPin) {
            throw "onnxruntime.dll SHA-256 mismatch: expected $dllPin, got $dllHash. Nothing was installed."
        }

        $files = @($stagedDll)
        foreach ($entry in $sharedEntries) { $files += Expand-ToStaging -Entry $entry -StagingDir $staging }
        foreach ($entry in $licenseEntries) { $files += Expand-ToStaging -Entry $entry -StagingDir $staging }
    }
    finally {
        $zip.Dispose()
    }

    Publish-Files -Files $files
    Write-Host "Installed verified ONNX Runtime 1.20.1 (cpu) to $runtimeDir"
}

function Get-DirectMlPins {
    $section = 'runtime.windows-x64.directml'
    $pins = Read-ManifestSection -Section $section
    foreach ($key in @('archive_sha256', 'dll_sha256', 'archive_size')) {
        $value = ''
        if ($pins.ContainsKey($key)) { $value = [string]$pins[$key] }
        if ($value -eq '' -or $value -eq 'TODO-compute') {
            throw "Manifest [$section] $key is still the placeholder. Run 'powershell -NoProfile -File tools\inference\fetch-windows-runtime.ps1 -Variant directml -ComputeOnly' and record the printed values in assets/model/manifest.toml first. Nothing was installed."
        }
    }
    return @{
        ArchiveSha256 = Get-ManifestPin -Values $pins -Section $section -Key 'archive_sha256'
        DllSha256     = Get-ManifestPin -Values $pins -Section $section -Key 'dll_sha256'
    }
}

function Select-DirectMlEntries {
    param($Zip)
    # Explicit allowlist of exact entry names. Every other package entry, including DirectML.dll, is ignored.
    $dllEntries = @($Zip.Entries | Where-Object { $_.FullName -ceq $directMlDllEntry })
    $dll = Get-SingleEntry -Entries $dllEntries -Description $directMlDllEntry
    $licenses = @()
    foreach ($name in $directMlLicenseEntries) {
        $found = @($Zip.Entries | Where-Object { $_.FullName -ceq $name })
        if ($found.Count -gt 1) {
            throw "Expected at most one $name in the package, found $($found.Count). Nothing was installed."
        }
        if ($found.Count -eq 1) {
            $licenses += $found[0]
        }
        elseif ($directMlRequiredLicenses -contains $name) {
            throw "Package has no $name. Nothing was installed."
        }
    }
    $allowed = @($directMlDllEntry) + $directMlLicenseEntries
    foreach ($entry in @($dll) + $licenses) {
        if ($allowed -cnotcontains $entry.FullName) {
            throw "Internal error: $($entry.FullName) is not on the DirectML allowlist. Nothing was installed."
        }
        if ($entry.Name -ieq 'DirectML.dll') {
            throw 'DirectML.dll must never be extracted. Nothing was installed.'
        }
    }
    return [pscustomobject]@{ Dll = $dll; Licenses = $licenses }
}

function Invoke-ComputeDirectMl {
    $work = New-EmptyTempDir
    $package = Join-Path $work 'Microsoft.ML.OnnxRuntime.DirectML.1.20.1.nupkg'
    Save-Download -Url $directMlUrl -Destination $package
    $archiveHash = Get-Sha256 $package
    $archiveSize = (Get-Item -LiteralPath $package).Length

    $zip = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $selected = Select-DirectMlEntries -Zip $zip
        $staging = New-EmptyTempDir
        $stagedDll = Expand-ToStaging -Entry $selected.Dll -StagingDir $staging
        $dllHash = Get-Sha256 $stagedDll
    }
    finally {
        $zip.Dispose()
    }

    Write-Host "variant:        directml (ONNX Runtime 1.20.1, NuGet package Microsoft.ML.OnnxRuntime.DirectML)"
    Write-Host "archive_sha256 = `"$archiveHash`""
    Write-Host "dll_sha256     = `"$dllHash`""
    Write-Host "archive size:   $archiveSize bytes"
}

function Install-DirectMl {
    $pins = Get-DirectMlPins
    $archivePin = $pins.ArchiveSha256
    $dllPin = $pins.DllSha256

    $targetDll = Join-Path $runtimeDir 'onnxruntime.dll'
    if (-not $Force -and (Test-Path -LiteralPath $targetDll) -and ((Get-Sha256 $targetDll) -eq $dllPin)) {
        Write-Host "Verified onnxruntime.dll already present in $runtimeDir. Nothing to do (use -Force to refetch)."
        return
    }

    $work = New-EmptyTempDir
    $package = Join-Path $work 'Microsoft.ML.OnnxRuntime.DirectML.1.20.1.nupkg'
    Save-Download -Url $directMlUrl -Destination $package
    $archiveHash = Get-Sha256 $package
    if ($archiveHash -ne $archivePin) {
        throw "Archive SHA-256 mismatch: expected $archivePin, got $archiveHash. Nothing was installed."
    }

    $staging = New-EmptyTempDir
    $zip = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $selected = Select-DirectMlEntries -Zip $zip

        $stagedDll = Expand-ToStaging -Entry $selected.Dll -StagingDir $staging
        $dllHash = Get-Sha256 $stagedDll
        if ($dllHash -ne $dllPin) {
            throw "onnxruntime.dll SHA-256 mismatch: expected $dllPin, got $dllHash. Nothing was installed."
        }

        $files = @($stagedDll)
        foreach ($entry in $selected.Licenses) { $files += Expand-ToStaging -Entry $entry -StagingDir $staging }
    }
    finally {
        $zip.Dispose()
    }

    foreach ($file in $files) {
        if ((Split-Path -Leaf $file) -ieq 'DirectML.dll') {
            throw 'DirectML.dll must never be installed. Nothing was installed.'
        }
    }

    Publish-Files -Files $files
    Write-Host "Installed verified ONNX Runtime 1.20.1 (directml) to $runtimeDir"
}

try {
    if ($Variant -eq 'cpu') {
        if ($ComputeOnly) { Invoke-ComputeCpu } else { Install-Cpu }
    }
    elseif ($ComputeOnly) {
        Invoke-ComputeDirectMl
    }
    else {
        Install-DirectMl
    }
    $exitCode = 0
}
catch {
    [Console]::Error.WriteLine('ERROR: ' + $_.Exception.Message)
    $exitCode = 1
}
finally {
    foreach ($dir in $script:TempDirs) {
        if (Test-Path -LiteralPath $dir) {
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

exit $exitCode
