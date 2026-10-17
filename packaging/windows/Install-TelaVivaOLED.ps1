<#
.SYNOPSIS
Installs TelaViva OLED for the current user. Administrator rights are not needed.

.DESCRIPTION
Run this script from the extracted release folder, the one that contains TelaViva.exe.
It copies the app to the install folder, creates a Start menu shortcut and registers the
app in Settings > Apps for the current user. Running it again upgrades the files in place.

It writes only these: the install folder, the Start menu shortcut, the HKCU uninstall key
and, with -LaunchAtLogin, the HKCU Run value. It stops only a TelaViva.exe that runs from
the install folder. It never touches the settings and logs folder.

.PARAMETER InstallDir
Install folder. Default: %LOCALAPPDATA%\Programs\TelaVivaOLED.

.PARAMETER NoStartMenu
Do not create the Start menu shortcut.

.PARAMETER LaunchAtLogin
Write the HKCU Run value that starts the app at sign-in, in the same form the app writes it.

.PARAMETER Force
Allow installing into an existing folder that does not contain TelaViva.exe.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\TelaVivaOLED'),

    [switch]$NoStartMenu,

    [switch]$LaunchAtLogin,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# These names must match the app and the uninstaller. The Run value name is kValueName in
# native/windows/Platform/LaunchAtLogin.cpp, and the command form is "<exe>" --background.
$appName = 'TelaViva OLED'
$exeName = 'TelaViva.exe'
$runKeyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'TelaVivaOLED'
$uninstallKeyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\TelaVivaOLED'
$shortcutName = 'TelaViva OLED.lnk'

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

function Test-IsUnder {
    param([string]$Path, [string]$Dir)
    $child = [System.IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    $parent = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\') + '\'
    return $child.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)
}

function Copy-Tree {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($child in @(Get-ChildItem -LiteralPath $Source -Force)) {
        $target = Join-Path $Destination $child.Name
        if ($child.PSIsContainer) {
            Copy-Tree -Source $child.FullName -Destination $target
        }
        else {
            Copy-Item -LiteralPath $child.FullName -Destination $target -Force
        }
    }
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
    $sourceDir = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $installFull = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if ([string]::IsNullOrEmpty($installFull) -or $installFull -match '^[A-Za-z]:$') {
        Fail "Refusing to install into a drive root: $InstallDir"
    }

    # The package must be complete. A partial copy would install an app that cannot start.
    $required = @(
        $exeName,
        'telaviva_core.dll',
        'telaviva_inference.dll',
        'Uninstall-TelaVivaOLED.ps1',
        'acceleration-profiles.toml',
        'selftest-face.bgra',
        'models\face_detection_yunet_2023mar.onnx'
    )
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $sourceDir $relative) -PathType Leaf)) {
            Fail "This is not a complete TelaViva OLED package; $relative is missing. Run the installer from the extracted zip folder."
        }
    }

    if ((Test-IsUnder -Path $sourceDir -Dir $installFull) -or (Test-IsUnder -Path $installFull -Dir $sourceDir)) {
        Fail "The install folder and the package folder must not overlap: $installFull"
    }

    if (Test-Path -LiteralPath $installFull) {
        if (-not (Test-Path -LiteralPath $installFull -PathType Container)) {
            Fail "The install path is a file: $installFull"
        }
        $hasItems = @(Get-ChildItem -LiteralPath $installFull -Force).Count -gt 0
        $isOurInstall = Test-Path -LiteralPath (Join-Path $installFull $exeName) -PathType Leaf
        if ($hasItems -and -not $isOurInstall -and -not $Force) {
            Fail "The folder is not empty and does not contain ${exeName}: $installFull. Choose another -InstallDir or pass -Force."
        }
    }

    $exePath = Join-Path $installFull $exeName
    $version = 'unknown'
    $versionFile = Join-Path $sourceDir 'VERSION'
    if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
        $versionText = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
        if (-not [string]::IsNullOrEmpty($versionText)) { $version = $versionText }
    }

    # Stop only the running app that comes from this install folder, so an upgrade can replace files.
    foreach ($proc in @(Get-AppProcessesFromDir -Dir $installFull)) {
        if ($PSCmdlet.ShouldProcess("$exeName (PID $($proc.Id))", 'Stop running app')) {
            Stop-Process -Id $proc.Id -Force
            Wait-Process -Id $proc.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
    }

    if ($PSCmdlet.ShouldProcess($installFull, 'Copy app files')) {
        try {
            Copy-Tree -Source $sourceDir -Destination $installFull
        }
        catch {
            Fail "Could not copy the files to $installFull ($($_.Exception.Message)). Quit $appName and run the installer again."
        }
    }

    if (-not $NoStartMenu) {
        $programsDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
        $shortcutPath = Join-Path $programsDir $shortcutName
        if ($PSCmdlet.ShouldProcess($shortcutPath, 'Create Start menu shortcut')) {
            New-Item -ItemType Directory -Force -Path $programsDir | Out-Null
            $shell = New-Object -ComObject WScript.Shell
            try {
                $shortcut = $shell.CreateShortcut($shortcutPath)
                $shortcut.TargetPath = $exePath
                $shortcut.WorkingDirectory = $installFull
                $shortcut.Description = $appName
                # The icon is resource 1 of TelaViva.exe, so index 0 selects it.
                $shortcut.IconLocation = "$exePath,0"
                $shortcut.Save()
            }
            finally {
                [System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null
            }
        }
    }

    if ($PSCmdlet.ShouldProcess($uninstallKeyPath, 'Write uninstall entry')) {
        New-Item -Path $uninstallKeyPath -Force | Out-Null
        # The publisher name is escaped so this script stays ASCII for Windows PowerShell 5.1.
        $publisher = "C$([char]0x00E1)ssio de Freitas e Silva"
        $uninstallScript = Join-Path $installFull 'Uninstall-TelaVivaOLED.ps1'
        $values = [ordered]@{
            DisplayName     = $appName
            DisplayVersion  = $version
            Publisher       = $publisher
            InstallLocation = $installFull
            DisplayIcon     = "$exePath,0"
            UninstallString = 'powershell -NoProfile -ExecutionPolicy Bypass -File "' + $uninstallScript + '"'
        }
        foreach ($name in $values.Keys) {
            New-ItemProperty -Path $uninstallKeyPath -Name $name -Value ([string]$values[$name]) -PropertyType String -Force | Out-Null
        }
        foreach ($name in @('NoModify', 'NoRepair')) {
            New-ItemProperty -Path $uninstallKeyPath -Name $name -Value 1 -PropertyType DWord -Force | Out-Null
        }
    }

    if ($LaunchAtLogin) {
        # Same form the app writes, so the app reports launch at login as enabled.
        $runCommand = '"' + $exePath + '" --background'
        if ($PSCmdlet.ShouldProcess($runKeyPath, "Set Run value $runValueName")) {
            New-ItemProperty -Path $runKeyPath -Name $runValueName -Value $runCommand -PropertyType String -Force | Out-Null
            $readBack = (Get-ItemProperty -Path $runKeyPath -Name $runValueName -ErrorAction Stop).$runValueName
            if ($readBack -cne $runCommand) { Fail 'The Run value did not match after writing.' }
        }
    }

    Write-Host ''
    Write-Host "$appName $version is installed in $installFull"
    if (-not $NoStartMenu) { Write-Host "Start menu shortcut: $shortcutName" }
    if ($LaunchAtLogin) { Write-Host 'Launch at sign-in: enabled (HKCU Run value)' }
    Write-Host 'Remove it with Uninstall-TelaVivaOLED.ps1 in the install folder, or from Settings > Apps.'
    exit 0
}
catch {
    Fail $_.Exception.Message
}
