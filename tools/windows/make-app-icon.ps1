<#
.SYNOPSIS
Generates the Windows application icon: a multi-size AppIcon.ico.

.DESCRIPTION
Generated from the assets/branding/liquid-glass layers (screen.svg, stand.svg, dot.svg).
The icon is the monitor only, by design: background.svg is not used and there is no
rounded-square tile (owner decision, 2026-10-17). This script is the source of truth for
the Windows icon composition: change the composition here, not in the output file.

Rasterization uses Microsoft Edge headless when available, and falls back to drawing the
layer geometry with System.Drawing when it is not.

Run from any directory on Windows PowerShell 5.1:
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\make-app-icon.ps1
#>
[CmdletBinding()]
param(
    [string]$OutFile = ''
)

Add-Type -AssemblyName System.Drawing

# Sizes shipped in the ICO. 256 is stored with width byte 0.
$IconSizes = @(16, 24, 32, 48, 64, 128, 256)

# Transparent margin on each side, as a fraction of the cropped monitor size.
$MarginRatio = 0.06

# Resolve paths in the body: $PSScriptRoot can be empty on Windows PowerShell 5.1.
$scriptDir = $PSScriptRoot
if ([string]::IsNullOrEmpty($scriptDir)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir '..\..'))
$layerDir = Join-Path $repoRoot 'assets\branding\liquid-glass'

if ([string]::IsNullOrEmpty($OutFile)) {
    $OutFile = Join-Path $repoRoot 'packaging\windows\Resources\AppIcon.ico'
}
if (-not [System.IO.Path]::IsPathRooted($OutFile)) {
    $OutFile = Join-Path (Get-Location).Path $OutFile
}
$OutFile = [System.IO.Path]::GetFullPath($OutFile)

# Copies a bitmap into a fresh 32bpp ARGB bitmap, releasing any file lock on the source.
function Copy-Bitmap {
    param([System.Drawing.Bitmap]$Source)

    $copy = New-Object System.Drawing.Bitmap -ArgumentList $Source.Width, $Source.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($copy)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $g.DrawImage($Source, 0, 0, $Source.Width, $Source.Height)
    }
    finally {
        $g.Dispose()
    }
    return $copy
}

# Renders the layers in order on a 1024 px transparent page with Edge headless.
# Returns $null when Edge is missing, fails, or produces an image smaller than 1024 px.
function Get-MasterWithEdge {
    param([string[]]$Layers)

    $edge = $null
    foreach ($candidate in @('C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', 'C:\Program Files\Microsoft\Edge\Application\msedge.exe')) {
        if (Test-Path -LiteralPath $candidate) {
            $edge = $candidate
            break
        }
    }
    if ($null -eq $edge) { return $null }

    $id = [guid]::NewGuid().ToString('N')
    $htmlPath = Join-Path $env:TEMP ('telaviva-app-icon-' + $id + '.html')
    $pngPath = Join-Path $env:TEMP ('telaviva-app-icon-' + $id + '.png')
    $profileDir = Join-Path $env:TEMP ('telaviva-app-icon-profile-' + $id)
    try {
        $imgs = ''
        foreach ($layer in $Layers) {
            $layerUri = (New-Object System.Uri -ArgumentList $layer).AbsoluteUri
            $imgs += '<img src="' + $layerUri + '" style="position:absolute;left:0;top:0;width:1024px;height:1024px">'
        }
        $html = '<!DOCTYPE html><html><head><meta charset="utf-8"><style>html,body{margin:0;background:transparent}</style></head><body>' + $imgs + '</body></html>'
        [System.IO.File]::WriteAllText($htmlPath, $html, (New-Object System.Text.UTF8Encoding($false)))
        $htmlUri = (New-Object System.Uri -ArgumentList $htmlPath).AbsoluteUri

        # A separate profile keeps headless from handing off to a running Edge instance.
        $edgeArgs = '--headless=new --disable-gpu --hide-scrollbars --no-first-run' +
            ' --default-background-color=00000000 --window-size=1024,1024' +
            ' --virtual-time-budget=5000 --user-data-dir="' + $profileDir + '"' +
            ' --screenshot="' + $pngPath + '" "' + $htmlUri + '"'
        $proc = Start-Process -FilePath $edge -ArgumentList $edgeArgs -PassThru -WindowStyle Hidden
        if (-not $proc.WaitForExit(120000)) {
            $proc.Kill()
            return $null
        }

        if (-not (Test-Path -LiteralPath $pngPath)) { return $null }
        $shot = [System.Drawing.Bitmap]::FromFile($pngPath)
        try {
            if ($shot.Width -lt 1024 -or $shot.Height -lt 1024) { return $null }
            return (Copy-Bitmap -Source $shot)
        }
        finally {
            $shot.Dispose()
        }
    }
    finally {
        Remove-Item -LiteralPath $htmlPath, $pngPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $profileDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Rounded rectangle path; $R = 0 gives a plain rectangle.
function New-RoundRectPath {
    param([single]$X, [single]$Y, [single]$W, [single]$H, [single]$R)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($R -le 0) {
        $path.AddRectangle([System.Drawing.RectangleF]::new($X, $Y, $W, $H))
        return $path
    }
    $d = 2 * $R
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# Fallback: draws the same geometry as screen.svg, stand.svg and dot.svg (1024 px grid).
function New-MasterWithDrawing {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList 1024, 1024, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $screenPath = $null
    $gradient = $null
    $standBrush = $null
    $standPath = $null
    $dotBrush = $null
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

        # screen.svg: rounded rect 242,304 540x350, rx 64. Gradient is objectBoundingBox
        # (0,0) to (1,1), so the brush is defined on the unit square and mapped to the box.
        $screenPath = New-RoundRectPath -X 242 -Y 304 -W 540 -H 350 -R 64
        $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList @(
            [System.Drawing.PointF]::new([single]0, [single]0),
            [System.Drawing.PointF]::new([single]1, [single]1),
            [System.Drawing.Color]::FromArgb(255, 255, 178, 36),
            [System.Drawing.Color]::FromArgb(255, 123, 63, 242))
        $blend = New-Object System.Drawing.Drawing2D.ColorBlend
        $blend.Colors = [System.Drawing.Color[]]@(
            [System.Drawing.Color]::FromArgb(255, 255, 178, 36),
            [System.Drawing.Color]::FromArgb(255, 255, 77, 106),
            [System.Drawing.Color]::FromArgb(255, 123, 63, 242))
        $blend.Positions = [single[]]@(0, 0.5, 1)
        $gradient.InterpolationColors = $blend
        $gradient.Transform = New-Object System.Drawing.Drawing2D.Matrix -ArgumentList @([single]540, [single]0, [single]0, [single]350, [single]242, [single]304)
        $g.FillPath($gradient, $screenPath)

        # stand.svg: neck rect 488,644 48x76 and base pill 396,710 232x30 rx 15.
        $standBrush = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::FromArgb(255, 233, 233, 239))
        $g.FillRectangle($standBrush, [single]488, [single]644, [single]48, [single]76)
        $standPath = New-RoundRectPath -X 396 -Y 710 -W 232 -H 30 -R 15
        $g.FillPath($standBrush, $standPath)

        # dot.svg: white circle at 512,479 r 54.
        $dotBrush = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::White)
        $g.FillEllipse($dotBrush, [single](512 - 54), [single](479 - 54), [single](2 * 54), [single](2 * 54))
    }
    finally {
        if ($null -ne $dotBrush) { $dotBrush.Dispose() }
        if ($null -ne $standPath) { $standPath.Dispose() }
        if ($null -ne $standBrush) { $standBrush.Dispose() }
        if ($null -ne $gradient) { $gradient.Dispose() }
        if ($null -ne $screenPath) { $screenPath.Dispose() }
        $g.Dispose()
    }
    return $bmp
}

# Bounding box of pixels with non-zero alpha.
function Get-AlphaBounds {
    param([System.Drawing.Bitmap]$Source)

    $rect = New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $Source.Width, $Source.Height
    $data = $Source.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $bytes = New-Object byte[] ($data.Stride * $Source.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    }
    finally {
        $Source.UnlockBits($data)
    }

    $minX = $Source.Width
    $minY = $Source.Height
    $maxX = -1
    $maxY = -1
    for ($y = 0; $y -lt $Source.Height; $y++) {
        $row = $y * $data.Stride
        for ($x = 0; $x -lt $Source.Width; $x++) {
            if ($bytes[$row + $x * 4 + 3] -gt 0) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                $maxY = $y
            }
        }
    }
    if ($maxX -lt 0) { throw 'The composed monitor has no visible pixels.' }
    return @{ X = $minX; Y = $minY; W = ($maxX - $minX + 1); H = ($maxY - $minY + 1) }
}

# Centers the cropped monitor on a transparent square with a margin on every side.
function New-SquareCanvas {
    param([System.Drawing.Bitmap]$Source, $Bounds)

    $content = [Math]::Max($Bounds.W, $Bounds.H)
    $margin = [int][Math]::Round($content * $MarginRatio)
    $side = $content + 2 * $margin
    $canvas = New-Object System.Drawing.Bitmap -ArgumentList $side, $side, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $dx = [int][Math]::Floor(($side - $Bounds.W) / 2)
        $dy = [int][Math]::Floor(($side - $Bounds.H) / 2)
        $srcRect = New-Object System.Drawing.Rectangle -ArgumentList $Bounds.X, $Bounds.Y, $Bounds.W, $Bounds.H
        $dstRect = New-Object System.Drawing.Rectangle -ArgumentList $dx, $dy, $Bounds.W, $Bounds.H
        $g.DrawImage($Source, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $g.Dispose()
    }
    return $canvas
}

# Bicubic resample of the square canvas to one size, returned as PNG bytes.
function New-SizedPng {
    param([System.Drawing.Bitmap]$Canvas, [int]$Size)

    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $ms = New-Object System.IO.MemoryStream
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.DrawImage($Canvas, 0, 0, $Size, $Size)
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$ms.ToArray()
    }
    finally {
        $ms.Dispose()
        $g.Dispose()
        $bmp.Dispose()
    }
}

# Writes an ICO container by hand: System.Drawing.Icon cannot save multi-size files.
# Each entry holds a PNG-compressed image, which Windows accepts for sizes of 256 or less.
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
            $entrySize = $Sizes[$i]
            $dim = $entrySize
            if ($entrySize -ge 256) { $dim = 0 }
            $png = [byte[]]$Pngs[$i]
            $bw.Write([byte]$dim)
            $bw.Write([byte]$dim)
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

$layers = @(
    (Join-Path $layerDir 'screen.svg'),
    (Join-Path $layerDir 'stand.svg'),
    (Join-Path $layerDir 'dot.svg')
)
foreach ($layer in $layers) {
    if (-not (Test-Path -LiteralPath $layer)) { throw ('Missing layer: ' + $layer) }
}

$master = $null
try {
    $master = Get-MasterWithEdge -Layers $layers
}
catch {
    $master = $null
}
if ($null -eq $master) {
    Write-Host 'Edge unavailable or failed; drawing the layers with System.Drawing.'
    $master = New-MasterWithDrawing
}

$bounds = Get-AlphaBounds -Source $master
$canvas = New-SquareCanvas -Source $master -Bounds $bounds
$master.Dispose()

$pngs = New-Object System.Collections.ArrayList
foreach ($size in $IconSizes) {
    [void]$pngs.Add((New-SizedPng -Canvas $canvas -Size $size))
}
$canvas.Dispose()

$outDir = Split-Path -Parent $OutFile
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Write-Ico -Path $OutFile -Sizes $IconSizes -Pngs $pngs

Write-Output ('Wrote ' + $OutFile)
Write-Output ('sizes: ' + ($IconSizes -join ', '))
