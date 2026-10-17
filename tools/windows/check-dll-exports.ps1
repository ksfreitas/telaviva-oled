<#
.SYNOPSIS
Checks that the tv_ functions exported by the Rust DLLs match the C headers
in include\ (telaviva_core.h and telaviva_inference.h).

.PARAMETER DllDir
Directory holding telaviva_core.dll and telaviva_inference.dll. Relative paths
resolve from the repository root. Defaults to native\windows\build\rust.
#>
[CmdletBinding()]
param(
    [string]$DllDir = ''
)

$ErrorActionPreference = 'Stop'

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

# This script lives in tools\windows, so the repository root is two levels up.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ([string]::IsNullOrEmpty($DllDir)) {
    $DllDir = Join-Path $repoRoot 'native\windows\build\rust'
}
elseif (-not [System.IO.Path]::IsPathRooted($DllDir)) {
    $DllDir = Join-Path $repoRoot $DllDir
}
$DllDir = [System.IO.Path]::GetFullPath($DllDir)

function Find-Dumpbin {
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrEmpty($programFilesX86)) { return $null }

    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return $null }

    $installPaths = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0) { return $null }

    foreach ($installPath in @($installPaths)) {
        if ([string]::IsNullOrEmpty($installPath)) { continue }
        $msvcRoot = Join-Path $installPath 'VC\Tools\MSVC'
        if (-not (Test-Path -LiteralPath $msvcRoot -PathType Container)) { continue }

        # Prefer the newest toolset; compare as versions so 14.10 sorts after 14.9.
        $toolsets = Get-ChildItem -LiteralPath $msvcRoot -Directory |
            Sort-Object -Property @{ Expression = {
                    try { [version]$_.Name } catch { [version]'0.0' }
                } } -Descending

        foreach ($toolset in $toolsets) {
            $candidate = Join-Path $toolset.FullName 'bin\Hostx64\x64\dumpbin.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    return $null
}

function Get-DllExportNames {
    param(
        [string]$Dumpbin,
        [string]$DllPath
    )

    $output = & $Dumpbin /exports $DllPath
    if ($LASTEXITCODE -ne 0) {
        Fail "dumpbin /exports failed for $DllPath with exit code $LASTEXITCODE."
    }

    # Export table rows: ordinal, hint, RVA (8 hex digits), name. dumpbin appends
    # ' = alias' to some names, so the pattern anchors on the row prefix only and
    # captures just the first name.
    $pattern = '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(tv_\w+)'
    $names = @()
    foreach ($line in @($output)) {
        if ($line -match $pattern) { $names += $Matches[1] }
    }
    return @($names | Sort-Object -Unique)
}

function Get-HeaderFunctionNames {
    param([string]$HeaderPath)

    $text = [System.IO.File]::ReadAllText($HeaderPath)
    # Strip comments and preprocessor lines (including backslash continuations)
    # first, so declarations are only matched against real C code.
    $text = [regex]::Replace($text, '/\*[\s\S]*?\*/', ' ')
    $text = [regex]::Replace($text, '//[^\r\n]*', '')
    $text = [regex]::Replace($text, '(?m)^[ \t]*#(?:[^\r\n]*\\\r?\n)*[^\r\n]*', '')

    $names = @()
    # A C declaration ends at ';'. Splitting on it handles declarations that span lines.
    foreach ($chunk in $text.Split(';')) {
        # Skip typedefs, including function-pointer typedefs and struct bodies.
        if ($chunk -match '\btypedef\b') { continue }
        # Drop anything up to the last brace so 'extern "C" {' and struct
        # bodies do not hide the return type of the following declaration.
        $decl = [regex]::Replace($chunk, '^[\s\S]*[{}]', '')
        # Require a return type before the name and an opening parenthesis after it.
        $m = [regex]::Match($decl, '^\s*[A-Za-z_][^(){}=]*?\b(tv_\w+)\s*\(')
        if ($m.Success) { $names += $m.Groups[1].Value }
    }
    return @($names | Sort-Object -Unique)
}

$dumpbin = Find-Dumpbin
if (-not $dumpbin) {
    Fail 'dumpbin.exe not found. Install the Visual Studio C++ build tools (VC.Tools.x86.x64) or run from a Developer shell.'
}

$pairs = @(
    @{ Dll = 'telaviva_core.dll'; Header = (Join-Path $repoRoot 'include\telaviva_core.h') },
    @{ Dll = 'telaviva_inference.dll'; Header = (Join-Path $repoRoot 'include\telaviva_inference.h') }
)

$totalChecked = 0
$totalProblems = 0
$summaryParts = @()

foreach ($pair in $pairs) {
    $dllPath = Join-Path $DllDir $pair.Dll
    $headerPath = $pair.Header

    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) { Fail "DLL not found: $dllPath" }
    if (-not (Test-Path -LiteralPath $headerPath -PathType Leaf)) { Fail "Header not found: $headerPath" }

    $dllNames = Get-DllExportNames -Dumpbin $dumpbin -DllPath $dllPath
    $headerNames = Get-HeaderFunctionNames -HeaderPath $headerPath

    $allNames = @($dllNames + $headerNames) | Sort-Object -Unique
    $rows = @()
    foreach ($name in $allNames) {
        $inHeader = $headerNames -contains $name
        $inDll = $dllNames -contains $name
        $status = 'OK'
        if ($inHeader -and -not $inDll) { $status = 'MISSING_IN_DLL' }
        elseif ($inDll -and -not $inHeader) { $status = 'EXTRA_IN_DLL' }
        $rows += [pscustomobject]@{ Name = $name; Status = $status }
    }

    $problems = @($rows | Where-Object { $_.Status -ne 'OK' }).Count
    Write-Host ''
    Write-Host "== $($pair.Dll) vs $(Split-Path -Leaf $headerPath) =="
    if ($rows.Count -gt 0) {
        $rows | Format-Table -AutoSize | Out-String | Write-Host
    }
    else {
        Write-Host 'No tv_ functions found in either the DLL or the header.'
    }

    $totalChecked += $dllNames.Count
    $totalProblems += $problems
    $summaryParts += "$($pair.Dll): $($headerNames.Count) header, $($dllNames.Count) exported, $problems mismatch(es)"
}

Write-Host ''
if ($totalProblems -gt 0) {
    Write-Host ('FAILED: ' + ($summaryParts -join '; '))
    exit 1
}

Write-Host ('OK: tv_ exports match the headers. ' + ($summaryParts -join '; '))
exit 0
