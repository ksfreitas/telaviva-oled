<#
.SYNOPSIS
Writes the DirectML self-test frame (selftest-face.bgra) from the CC0 face fixture.

.DESCRIPTION
Decodes crates\telaviva-inference\tests\fixtures\cc0-face.ppm (binary P6, square portrait),
scales the face to the full 240-pixel frame height and centres it in a 320x240 black
letterbox. The file layout, read by packaging\windows\build-app.ps1 and the app:

  magic    4 bytes   "TVSF"
  width    uint32    little endian (320)
  height   uint32    little endian (240)
  stride   uint32    little endian (width * 4)
  pixels   stride * height bytes, top-down BGRA8, alpha 255

Uses System.Drawing, so it runs on Windows PowerShell 5.1.

.PARAMETER Out
Output file. Empty means native\windows\build\selftest\selftest-face.bgra under the
repository root. Relative paths resolve from the repository root. The folder is created.
#>
[CmdletBinding()]
param(
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'

function Fail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

# This script lives in tools\windows, so the repository root is two levels up.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

$fixture = Join-Path $repoRoot 'crates\telaviva-inference\tests\fixtures\cc0-face.ppm'
if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) { Fail "CC0 face fixture is missing: $fixture" }

if ([string]::IsNullOrEmpty($Out)) {
    $outPath = Join-Path $repoRoot 'native\windows\build\selftest\selftest-face.bgra'
}
elseif (-not [System.IO.Path]::IsPathRooted($Out)) {
    $outPath = Join-Path $repoRoot $Out
}
else {
    $outPath = $Out
}
$outPath = [System.IO.Path]::GetFullPath($outPath)

$frameWidth = 320
$frameHeight = 240
$faceSide = 240
$faceX = [int](($frameWidth - $faceSide) / 2)
$rowBytes = $frameWidth * 4

Add-Type -AssemblyName System.Drawing

try {
    # Parses the PPM header: magic, width, height and maxval, skipping whitespace and '#' comments.
    $script:ppm = [System.IO.File]::ReadAllBytes($fixture)
    $script:ppmPos = 0

    function Read-PpmToken {
        $bytes = $script:ppm
        while ($true) {
            if ($script:ppmPos -ge $bytes.Length) { Fail 'PPM header ends early.' }
            $c = $bytes[$script:ppmPos]
            if ($c -eq 0x23) {
                while ($script:ppmPos -lt $bytes.Length -and $bytes[$script:ppmPos] -ne 0x0A) { $script:ppmPos++ }
            }
            elseif ($c -eq 0x20 -or $c -eq 0x09 -or $c -eq 0x0A -or $c -eq 0x0D) {
                $script:ppmPos++
            }
            else {
                break
            }
        }
        $start = $script:ppmPos
        while ($script:ppmPos -lt $bytes.Length) {
            $c = $bytes[$script:ppmPos]
            if ($c -eq 0x20 -or $c -eq 0x09 -or $c -eq 0x0A -or $c -eq 0x0D) { break }
            $script:ppmPos++
        }
        return [System.Text.Encoding]::ASCII.GetString($bytes, $start, $script:ppmPos - $start)
    }

    $magic = Read-PpmToken
    if ($magic -ne 'P6') { Fail "Fixture is not a binary PPM (P6): $fixture" }
    $srcWidth = [int](Read-PpmToken)
    $srcHeight = [int](Read-PpmToken)
    $maxValue = [int](Read-PpmToken)
    if ($srcWidth -le 0 -or $srcHeight -le 0) { Fail 'Fixture has a zero dimension.' }
    if ($maxValue -ne 255) { Fail "Fixture maxval must be 255, found $maxValue." }

    # One whitespace byte separates the header from the RGB pixels.
    $script:ppmPos++
    $pixelStart = $script:ppmPos
    $pixelCount = $srcWidth * $srcHeight
    if (($script:ppm.Length - $pixelStart) -lt ($pixelCount * 3)) { Fail 'Fixture pixel data is truncated.' }

    # Convert RGB to top-down BGRA8 with opaque alpha.
    $srcBgra = [byte[]]::new($pixelCount * 4)
    $p = $pixelStart
    $o = 0
    for ($i = 0; $i -lt $pixelCount; $i++) {
        $srcBgra[$o] = $script:ppm[$p + 2]
        $srcBgra[$o + 1] = $script:ppm[$p + 1]
        $srcBgra[$o + 2] = $script:ppm[$p]
        $srcBgra[$o + 3] = 255
        $p += 3
        $o += 4
    }

    $source = New-Object System.Drawing.Bitmap($srcWidth, $srcHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $frame = New-Object System.Drawing.Bitmap($frameWidth, $frameHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $srcRect = New-Object System.Drawing.Rectangle(0, 0, $srcWidth, $srcHeight)
        $srcLock = $source.LockBits($srcRect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            if ($srcLock.Stride -ne ($srcWidth * 4)) { Fail 'Unexpected source stride.' }
            [System.Runtime.InteropServices.Marshal]::Copy($srcBgra, 0, $srcLock.Scan0, $srcBgra.Length)
        }
        finally {
            $source.UnlockBits($srcLock)
        }

        # Letterbox: black background, face scaled to the full frame height and centred.
        $g = [System.Drawing.Graphics]::FromImage($frame)
        try {
            $g.Clear([System.Drawing.Color]::Black)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $dest = New-Object System.Drawing.Rectangle($faceX, 0, $faceSide, $faceSide)
            $g.DrawImage($source, $dest)
        }
        finally {
            $g.Dispose()
        }

        $frameRect = New-Object System.Drawing.Rectangle(0, 0, $frameWidth, $frameHeight)
        $frameLock = $frame.LockBits($frameRect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            if ($frameLock.Stride -ne $rowBytes) { Fail 'Unexpected frame stride.' }
            $pixels = [byte[]]::new($rowBytes * $frameHeight)
            [System.Runtime.InteropServices.Marshal]::Copy($frameLock.Scan0, $pixels, 0, $pixels.Length)
        }
        finally {
            $frame.UnlockBits($frameLock)
        }
    }
    finally {
        $source.Dispose()
        $frame.Dispose()
    }

    # Header: magic, then width, height and stride as little-endian uint32.
    $header = [byte[]]::new(16)
    $header[0] = 0x54
    $header[1] = 0x56
    $header[2] = 0x53
    $header[3] = 0x46
    $fields = @([uint32]$frameWidth, [uint32]$frameHeight, [uint32]$rowBytes)
    for ($k = 0; $k -lt 3; $k++) {
        $value = $fields[$k]
        $at = 4 + 4 * $k
        $header[$at] = [byte]($value -band 0xFF)
        $header[$at + 1] = [byte](($value -shr 8) -band 0xFF)
        $header[$at + 2] = [byte](($value -shr 16) -band 0xFF)
        $header[$at + 3] = [byte](($value -shr 24) -band 0xFF)
    }

    $output = [byte[]]::new(16 + $pixels.Length)
    [System.Array]::Copy($header, 0, $output, 0, 16)
    [System.Array]::Copy($pixels, 0, $output, 16, $pixels.Length)

    $outDir = Split-Path -Parent $outPath
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    [System.IO.File]::WriteAllBytes($outPath, $output)
    Write-Host "Wrote self-test frame: $outPath ($($output.Length) bytes, ${frameWidth}x${frameHeight})"
    exit 0
}
catch {
    Fail "make-selftest-frame.ps1 failed: $($_.Exception.Message)"
}
