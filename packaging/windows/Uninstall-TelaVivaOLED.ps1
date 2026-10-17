<#
.SYNOPSIS
Removes TelaViva OLED for the current user.

.DESCRIPTION
Stops TelaViva.exe only when it runs from the install folder. Then it removes the HKCU Run
value that launch at login writes, the Start menu shortcut, the Settings > Apps entry and the
install folder. The settings, calibration and logs folder (%LOCALAPPDATA%\TelaVivaOLED) is
kept unless -RemoveUserData is given. Nothing else is changed.

When this script runs from inside the install folder, it copies itself to %TEMP%, runs that
copy, waits for it and returns its exit code. The install folder can then be deleted.

Exit codes: 0 when every step succeeded (or the WhatIf preview finished), 1 when a step failed.
Each failed step is named on stderr.

.PARAMETER InstallDir
Install folder. Default: the folder this script is in, when it contains TelaViva.exe,
otherwise %LOCALAPPDATA%\Programs\TelaVivaOLED.

.PARAMETER RemoveUserData
Also delete %LOCALAPPDATA%\TelaVivaOLED. Interactive runs ask for a typed DELETE first.
With -Quiet the switch pair is the consent, and nothing is asked.

.PARAMETER Quiet
No prompts and no pause at the end. The relaunched copy runs in a hidden window.

.PARAMETER Relaunched
Internal. Set when the temporary copy runs the uninstall.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir = '',

    [switch]$RemoveUserData,

    [switch]$Quiet,

    [switch]$Relaunched
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# These names must match the app and the installer. The Run value name is kValueName in
# native/windows/Platform/LaunchAtLogin.cpp. The data folder is the one AppPaths.cpp uses.
$appName = 'TelaViva OLED'
$exeName = 'TelaViva.exe'
$runKeyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'TelaVivaOLED'
$uninstallKeyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\TelaVivaOLED'
$shortcutName = 'TelaViva OLED.lnk'

# Removal steps keep going after a failure, so one run reports every step that failed.
$failures = New-Object System.Collections.Generic.List[string]

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

function Add-Failure {
    param([string]$Step, [string]$Message)
    $script:failures.Add("$Step ($Message)")
    [Console]::Error.WriteLine("ERROR: $Step failed: $Message")
}

function Test-IsUnder {
    param([string]$Path, [string]$Dir)
    $child = [System.IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    $parent = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\') + '\'
    return $child.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)
}

function Get-AppProcessesFromDir {
    param([string]$Dir)
    $found = @()
    foreach ($proc in @(Get-Process -Name 'TelaViva' -ErrorAction SilentlyContinue)) {
        $procPath = $null
        try { $procPath = $proc.Path } catch { $procPath = $null }
        if ($procPath -and (Test-IsUnder -Path $procPath -Dir $Dir)) { $found += $proc }
    }
    return $found
}

try {
    # Leave the install folder first: a process whose current directory is inside it blocks its deletion.
    # Set-Location moves only the PowerShell location; SetCurrentDirectory also releases the process cwd.
    if ($Relaunched) {
        Set-Location -LiteralPath $env:TEMP
        [System.IO.Directory]::SetCurrentDirectory($env:TEMP)
    }
    $scriptPath = [System.IO.Path]::GetFullPath($PSCommandPath)
    $scriptDir = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')

    if ([string]::IsNullOrEmpty($InstallDir)) {
        if (Test-Path -LiteralPath (Join-Path $scriptDir $exeName) -PathType Leaf) {
            $InstallDir = $scriptDir
        }
        else {
            $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\TelaVivaOLED'
        }
    }
    $installFull = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if ([string]::IsNullOrEmpty($installFull) -or $installFull -match '^[A-Za-z]:$') {
        Fail "Refusing to remove a drive root: $InstallDir"
    }

    # Only remove a folder that is clearly a TelaViva OLED install.
    if (Test-Path -LiteralPath $installFull) {
        if (-not (Test-Path -LiteralPath $installFull -PathType Container)) {
            Fail "The install path is a file, nothing was removed: $installFull"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $installFull $exeName) -PathType Leaf)) {
            Fail "The folder does not contain $exeName, so it is not removed: $installFull"
        }
    }

    # Running from inside the install folder would block its deletion, so run a temporary copy and
    # wait for it. The copy gets the same parameters, so WhatIf, Quiet and RemoveUserData still apply.
    if (-not $Relaunched -and (Test-IsUnder -Path $scriptPath -Dir $installFull)) {
        $tempCopy = Join-Path ([System.IO.Path]::GetTempPath()) ('TelaVivaOLED-Uninstall-' + [guid]::NewGuid().ToString('N') + '.ps1')
        Set-Location -LiteralPath $env:TEMP
        # Same reason as above: the parent must not pin the install folder while it waits.
        [System.IO.Directory]::SetCurrentDirectory($env:TEMP)
        try {
            Copy-Item -LiteralPath $scriptPath -Destination $tempCopy
        }
        catch {
            Fail "Could not copy the uninstaller to the temp folder: $($_.Exception.Message)"
        }
        $childArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -InstallDir "{1}" -Relaunched' -f $tempCopy, $installFull
        if ($RemoveUserData) { $childArgs += ' -RemoveUserData' }
        if ($Quiet) { $childArgs += ' -Quiet' }
        if ($WhatIfPreference) { $childArgs += ' -WhatIf' }
        $startParams = @{ FilePath = 'powershell.exe'; ArgumentList = $childArgs; WorkingDirectory = $env:TEMP; Wait = $true; PassThru = $true }
        if ($Quiet) {
            $startParams['WindowStyle'] = 'Hidden'
        }
        else {
            Write-Host 'Running the uninstall from a temporary copy.'
        }
        $child = Start-Process @startParams
        # The copy has exited by now, so it can be removed. Best effort only.
        try { Remove-Item -LiteralPath $tempCopy -Force -ErrorAction Stop } catch { }
        exit $child.ExitCode
    }

    # Stop the app only when it runs from this install folder.
    try {
        foreach ($proc in @(Get-AppProcessesFromDir -Dir $installFull)) {
            if ($PSCmdlet.ShouldProcess("$exeName (PID $($proc.Id))", 'Stop running app')) {
                Stop-Process -Id $proc.Id -Force
                Wait-Process -Id $proc.Id -Timeout 10 -ErrorAction SilentlyContinue
            }
        }
    }
    catch {
        Add-Failure 'Stop TelaViva.exe' $_.Exception.Message
    }

    # Remove only the value the app writes. Its name is exact, so an unrelated Run entry is never touched.
    try {
        $runItem = Get-ItemProperty -Path $runKeyPath -Name $runValueName -ErrorAction SilentlyContinue
        if ($runItem -and ($runItem.PSObject.Properties.Name -contains $runValueName)) {
            if ($PSCmdlet.ShouldProcess("$runKeyPath\$runValueName", 'Remove launch at login value')) {
                Remove-ItemProperty -Path $runKeyPath -Name $runValueName -ErrorAction Stop
            }
        }
    }
    catch {
        Add-Failure 'Remove the launch-at-login value' $_.Exception.Message
    }

    try {
        $programsDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
        $shortcutPath = Join-Path $programsDir $shortcutName
        if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
            if ($PSCmdlet.ShouldProcess($shortcutPath, 'Remove Start menu shortcut')) {
                Remove-Item -LiteralPath $shortcutPath -Force
            }
        }
    }
    catch {
        Add-Failure 'Remove the Start menu shortcut' $_.Exception.Message
    }

    try {
        if (Test-Path -LiteralPath $uninstallKeyPath) {
            if ($PSCmdlet.ShouldProcess($uninstallKeyPath, 'Remove uninstall entry')) {
                Remove-Item -LiteralPath $uninstallKeyPath -Recurse -Force
            }
        }
    }
    catch {
        Add-Failure 'Remove the Settings > Apps entry' $_.Exception.Message
    }

    # Retry briefly: antivirus scans and handle releases can hold files for a moment after the app stops.
    if (Test-Path -LiteralPath $installFull -PathType Container) {
        if ($PSCmdlet.ShouldProcess($installFull, 'Remove install folder')) {
            $removed = $false
            $lastError = ''
            for ($attempt = 1; $attempt -le 5 -and -not $removed; $attempt++) {
                try {
                    Remove-Item -LiteralPath $installFull -Recurse -Force -ErrorAction Stop
                    $removed = $true
                }
                catch {
                    $lastError = $_.Exception.Message
                    if ($attempt -lt 5) { Start-Sleep -Milliseconds 500 }
                }
            }
            if (-not $removed) {
                Add-Failure 'Remove the install folder' "$installFull could not be removed after 5 attempts: $lastError Quit $appName and run the uninstaller again."
            }
        }
    }

    $dataDir = Join-Path $env:LOCALAPPDATA 'TelaVivaOLED'
    try {
        if ($RemoveUserData) {
            if (Test-Path -LiteralPath $dataDir -PathType Container) {
                if ($WhatIfPreference) {
                    Write-Host "What if: would delete $dataDir"
                }
                elseif ($Quiet) {
                    Remove-Item -LiteralPath $dataDir -Recurse -Force
                    Write-Host "Deleted: $dataDir"
                }
                else {
                    $answer = Read-Host "Delete the settings, calibration and logs in $dataDir? Type DELETE to confirm"
                    if ($answer -ceq 'DELETE') {
                        Remove-Item -LiteralPath $dataDir -Recurse -Force
                        Write-Host "Deleted: $dataDir"
                    }
                    else {
                        Write-Host "Kept: $dataDir"
                    }
                }
            }
        }
        elseif (Test-Path -LiteralPath $dataDir -PathType Container) {
            Write-Host "Kept settings and logs: $dataDir (pass -RemoveUserData to delete them)"
        }
    }
    catch {
        Add-Failure 'Remove the settings and logs' $_.Exception.Message
    }

    Write-Host ''
    if ($failures.Count -gt 0) {
        [Console]::Error.WriteLine('ERROR: The uninstall did not finish. Failed steps: ' + ($failures -join '; '))
        $exitCode = 1
    }
    elseif ($WhatIfPreference) {
        Write-Host 'Preview finished. Nothing was changed.'
        $exitCode = 0
    }
    else {
        Write-Host "$appName was removed."
        $exitCode = 0
    }

    if ($Relaunched -and -not $Quiet) {
        Read-Host 'Press Enter to close this window' | Out-Null
    }
    exit $exitCode
}
catch {
    Fail $_.Exception.Message
}
