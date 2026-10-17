<#
.SYNOPSIS
Generates the Windows tray icon sets: six multi-size .ico files.

.DESCRIPTION
The glyphs are generated from the menu-bar SVG masters in assets/branding/menubar
(presence-absent.svg, presence-detected.svg, presence-dark.svg). This script is the
source of truth for the tray glyph geometry: change the geometry here, and keep the
SVGs in step when the glyph design changes.

Run from any directory on Windows PowerShell 5.1:
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\make-tray-icons.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir = ''
)

Add-Type -AssemblyName System.Drawing

# Sizes shipped in every .ico; Windows picks the closest one for the tray DPI.
$Sizes = @(16, 20, 24, 32, 48)

# Draws one glyph on a transparent square. The SVG grid is 18 x 18 units and every
# coordinate is multiplied by size / 18, so strokes keep their proportions at each size.
function New-GlyphPng {
    param(
        [int]$Size,
        [string]$State,
        [int]$Gray
    )

    $k = [double]$Size / 18.0
    $ink = [System.Drawing.Color]::FromArgb(255, $Gray, $Gray, $Gray)
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $pen = New-Object System.Drawing.Pen -ArgumentList $ink, ([single](1.5 * $k))
    $brush = New-Object System.Drawing.SolidBrush -ArgumentList $ink
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $ringPen = $null
    $ms = New-Object System.IO.MemoryStream
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        # Anti-aliasing keeps the 1.5 unit strokes smooth at 16 px.
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

        # Screen: rect x=1.5 y=2.5 w=15 h=10 rx=2.2, outline only.
        $x0 = 1.5 * $k
        $y0 = 2.5 * $k
        $x1 = 16.5 * $k
        $y1 = 12.5 * $k
        $r = 2.2 * $k
        $d = 2.0 * $r
        $path.AddArc([single]$x0, [single]$y0, [single]$d, [single]$d, 180, 90)
        $path.AddLine([single]($x0 + $r), [single]$y0, [single]($x1 - $r), [single]$y0)
        $path.AddArc([single]($x1 - $d), [single]$y0, [single]$d, [single]$d, 270, 90)
        $path.AddLine([single]$x1, [single]($y0 + $r), [single]$x1, [single]($y1 - $r))
        $path.AddArc([single]($x1 - $d), [single]($y1 - $d), [single]$d, [single]$d, 0, 90)
        $path.AddLine([single]($x1 - $r), [single]$y1, [single]($x0 + $r), [single]$y1)
        $path.AddArc([single]$x0, [single]($y1 - $d), [single]$d, [single]$d, 90, 90)
        $path.CloseFigure()
        $g.DrawPath($pen, $path)

        # Stand: M9 12.5 V15.5 and M5.5 15.5 H12.5, round caps as in the SVG.
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $g.DrawLine($pen, [single](9 * $k), [single](12.5 * $k), [single](9 * $k), [single](15.5 * $k))
        $g.DrawLine($pen, [single](5.5 * $k), [single](15.5 * $k), [single](12.5 * $k), [single](15.5 * $k))

        $cx = 9.0 * $k
        $cy = 7.5 * $k
        if ($State -eq 'detected') {
            # Filled dot: a face is present. Radius 2.2 units.
            $rr = 2.2 * $k
            $g.FillEllipse($brush, [single]($cx - $rr), [single]($cy - $rr), [single](2.0 * $rr), [single](2.0 * $rr))
        }
        if ($State -eq 'lowlight') {
            # Empty ring: insufficient light. Radius 2 units, stroke 1.1 units.
            $ringPen = New-Object System.Drawing.Pen -ArgumentList $ink, ([single](1.1 * $k))
            $rr = 2.0 * $k
            $g.DrawEllipse($ringPen, [single]($cx - $rr), [single]($cy - $rr), [single](2.0 * $rr), [single](2.0 * $rr))
        }
        # 'absent' draws no dot: no face, or protection off.

        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$ms.ToArray()
    }
    finally {
        if ($null -ne $ringPen) { $ringPen.Dispose() }
        $path.Dispose()
        $brush.Dispose()
        $pen.Dispose()
        $g.Dispose()
        $bmp.Dispose()
        $ms.Dispose()
    }
}

# Writes an ICO container by hand: System.Drawing.Icon cannot save multi-size files.
# Each entry holds a PNG-compressed image, which Windows accepts for sizes of 256 or less
# and which keeps the alpha channel.
function Write-Ico {
    param(
        [string]$Path,
        [int[]]$Sizes,
        [System.Collections.ArrayList]$Pngs
    )

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter -ArgumentList $ms
    try {
        # ICONDIR
        $bw.Write([uint16]0)
        $bw.Write([uint16]1)
        $bw.Write([uint16]$Sizes.Count)

        # ICONDIRENTRY per image; image data starts after the directory.
        $offset = 6 + 16 * $Sizes.Count
        for ($i = 0; $i -lt $Sizes.Count; $i++) {
            $size = $Sizes[$i]
            $png = [byte[]]$Pngs[$i]
            $bw.Write([byte]$size)
            $bw.Write([byte]$size)
            $bw.Write([byte]0)
            $bw.Write([byte]0)
            $bw.Write([uint16]1)
            $bw.Write([uint16]32)
            $bw.Write([uint32]$png.Length)
            $bw.Write([uint32]$offset)
            $offset += $png.Length
        }

        foreach ($png in $Pngs) {
            $bw.Write([byte[]]$png)
        }
        $bw.Flush()
        [System.IO.File]::WriteAllBytes($Path, $ms.ToArray())
    }
    finally {
        $bw.Dispose()
        $ms.Dispose()
    }
}

# Resolve the default here: $PSScriptRoot can be empty on Windows PowerShell 5.1.
if ([string]::IsNullOrEmpty($OutDir)) {
    $scriptDir = $PSScriptRoot
    if ([string]::IsNullOrEmpty($scriptDir)) {
        $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $OutDir = Join-Path $scriptDir '..\..\native\windows\TelaViva\Assets'
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Light variants are for a light taskbar (near-black #1F1F1F); dark variants for a dark
# taskbar (white).
$themes = @(
    @{ Name = 'light'; Gray = 31 },
    @{ Name = 'dark'; Gray = 255 }
)
$glyphs = @(
    @{ Name = 'detected'; State = 'detected' },
    @{ Name = 'absent'; State = 'absent' },
    @{ Name = 'lowlight'; State = 'lowlight' }
)

foreach ($glyph in $glyphs) {
    foreach ($theme in $themes) {
        $fileName = 'tray-' + $glyph.Name + '-' + $theme.Name + '.ico'
        $pngs = New-Object System.Collections.ArrayList
        foreach ($size in $Sizes) {
            [void]$pngs.Add((New-GlyphPng -Size $size -State $glyph.State -Gray $theme.Gray))
        }
        $target = Join-Path $OutDir $fileName
        Write-Ico -Path $target -Sizes $Sizes -Pngs $pngs
        Write-Output ($fileName + '  sizes: ' + ($Sizes -join ', '))
    }
}
