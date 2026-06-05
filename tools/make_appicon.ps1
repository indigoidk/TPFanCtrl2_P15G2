# Generates a flat, Fluent-style app icon (fan glyph on a rounded accent tile)
# at multiple resolutions and assembles them into a PNG-compressed .ico.
# Usage:  powershell -ExecutionPolicy Bypass -File tools\make_appicon.ps1 [outIcoPath] [previewPngPath]
param(
    [string]$OutIco  = (Join-Path $PSScriptRoot "..\res\app.ico"),
    [string]$Preview = (Join-Path $env:TEMP "appicon_preview.png")
)
Add-Type -AssemblyName System.Drawing

# ---- palette -------------------------------------------------------------
$accent = [System.Drawing.Color]::FromArgb(255, 0x11, 0x7D, 0xBB)   # flat cool blue
$blade  = [System.Drawing.Color]::FromArgb(255, 0xFF, 0xFF, 0xFF)   # white fan
$hubClr = [System.Drawing.Color]::FromArgb(255, 0x11, 0x7D, 0xBB)   # hub = accent (cut-out look)

function New-FanBitmap([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded-rect tile (full bleed, slight inset so the corners read at small sizes)
    $inset  = [Math]::Max(1, [int]($S * 0.02))
    $radius = [single]($S * 0.22)
    $rectF  = New-Object System.Drawing.RectangleF($inset, $inset, ($S - 2*$inset), ($S - 2*$inset))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rectF.X, $rectF.Y, $d, $d, 180, 90)
    $path.AddArc($rectF.Right - $d, $rectF.Y, $d, $d, 270, 90)
    $path.AddArc($rectF.Right - $d, $rectF.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rectF.X, $rectF.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $tile = New-Object System.Drawing.SolidBrush($accent)
    $g.FillPath($tile, $path)

    # fan: N broad, swept blades built as smooth closed curves (impeller look)
    $cx = $S / 2.0; $cy = $S / 2.0
    $bladeBrush = New-Object System.Drawing.SolidBrush($blade)
    $nBlades = 5
    $ri   = $S * 0.135      # inner (hub) radius
    $ro   = $S * 0.40       # outer (tip)  radius
    $hwIn = 0.30            # half angular width at the hub  (rad)
    $hwOut= 0.46            # half angular width at the tip  (rad)
    $skew = 0.42            # tip rotated vs hub -> swept "spinning" look (rad)
    function Pt([double]$cx,[double]$cy,[double]$r,[double]$a) {
        New-Object System.Drawing.PointF([single]($cx + $r*[Math]::Cos($a)), [single]($cy + $r*[Math]::Sin($a)))
    }
    for ($b = 0; $b -lt $nBlades; $b++) {
        $a = ($b / $nBlades) * 2 * [Math]::PI
        $pts = @(
            (Pt $cx $cy $ri  ($a - $hwIn)),
            (Pt $cx $cy ($ro*0.82) ($a + $skew - $hwOut)),
            (Pt $cx $cy $ro  ($a + $skew)),
            (Pt $cx $cy ($ro*0.82) ($a + $skew + $hwOut)),
            (Pt $cx $cy $ri  ($a + $hwIn))
        )
        $bp = New-Object System.Drawing.Drawing2D.GraphicsPath
        $bp.AddClosedCurve([System.Drawing.PointF[]]$pts, [single]0.45)
        $g.FillPath($bladeBrush, $bp)
        $bp.Dispose()
    }
    # centre hub: small accent-coloured disc punched out of the blades
    $hubR = $S * 0.085
    $hubBrush = New-Object System.Drawing.SolidBrush($hubClr)
    $g.FillEllipse($hubBrush, [single]($cx-$hubR), [single]($cy-$hubR), [single]($hubR*2), [single]($hubR*2))
    # tiny white centre dot
    $dotR = $S * 0.03
    $g.FillEllipse($bladeBrush, [single]($cx-$dotR), [single]($cy-$dotR), [single]($dotR*2), [single]($dotR*2))

    $g.Dispose()
    return $bmp
}

$sizes = @(16,24,32,48,64,128,256)
$pngs = @()
foreach ($s in $sizes) {
    $bm = New-FanBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bm.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,($ms.ToArray())
    $bm.Dispose(); $ms.Dispose()
}

# ---- assemble ICO (PNG-compressed entries) -------------------------------
$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i=0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($(if ($s -ge 256){0}else{$s})))   # width
    $bw.Write([byte]($(if ($s -ge 256){0}else{$s})))   # height
    $bw.Write([byte]0); $bw.Write([byte]0)             # colors, reserved
    $bw.Write([uint16]1); $bw.Write([uint16]32)        # planes, bpp
    $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($OutIco, $fs.ToArray())
$bw.Dispose(); $fs.Dispose()

# ---- preview sheet: 256 tile + a 16/32/48 strip on a checker ground ------
$pw = 256; $ph = 320
$pv = New-Object System.Drawing.Bitmap($pw, $ph)
$pg = [System.Drawing.Graphics]::FromImage($pv)
$pg.Clear([System.Drawing.Color]::FromArgb(255,40,40,40))
$big = New-FanBitmap 256
$pg.DrawImage($big, 0, 0, 256, 256); $big.Dispose()
$x = 8
foreach ($s in @(48,32,24,16)) {
    $b = New-FanBitmap $s
    $pg.DrawImage($b, $x, 262, $s, $s)
    $x += $s + 12
    $b.Dispose()
}
$pg.Dispose()
$pv.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)
$pv.Dispose()
Write-Output "ico=$OutIco preview=$Preview"
