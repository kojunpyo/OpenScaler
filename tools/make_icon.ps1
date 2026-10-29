# OpenScaler 로고와 아이콘 생성기
#
# 디자인
#   - 연분홍 배경 위의 하얀 행성 고리 (OpenScaler의 첫 글자 O)
#   - 가운데를 30도 기울어져 가로지르는 진한 분홍색 선
#   - 선을 경계로 왼쪽은 노이즈 낀 저해상도 이미지, 오른쪽은 선명한 이미지 (업스케일 전/후)
#
# 출력
#   assets/openscaler.ico  : exe 아이콘 (16~256px)
#   assets/logo.png        : README용 512px 로고
#
# 실행: powershell -ExecutionPolicy Bypass -File tools/make_icon.ps1

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$assets = Join-Path $root "assets"
New-Item -ItemType Directory -Force $assets | Out-Null

function Hex($h, $a = 255) {
    [System.Drawing.Color]::FromArgb($a, [Convert]::ToInt32($h.Substring(1, 2), 16),
        [Convert]::ToInt32($h.Substring(3, 2), 16), [Convert]::ToInt32($h.Substring(5, 2), 16))
}

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function GetPixels($bmp) {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, "ReadOnly", "Format32bppArgb")
    $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    return $bytes
}

function SetPixels($bmp, $bytes) {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, "WriteOnly", "Format32bppArgb")
    [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
    $bmp.UnlockBits($data)
}

# 선명한 원본 그림: 연분홍 배경, 별, 행성 고리
function DrawScene([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.PixelOffsetMode = "HighQuality"

    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0),
        (New-Object System.Drawing.PointF 0, $S), (Hex "#FFE3EE"), (Hex "#F5AFC8")
    $g.FillRectangle($bg, 0, 0, $S, $S)

    $stars = @(@(0.17, 0.19, 0.014), @(0.79, 0.15, 0.018), @(0.87, 0.63, 0.011), @(0.24, 0.81, 0.013),
               @(0.63, 0.86, 0.011), @(0.11, 0.52, 0.010), @(0.55, 0.11, 0.010), @(0.38, 0.30, 0.008))
    $starBrush = New-Object System.Drawing.SolidBrush (Hex "#FFFFFF" 220)
    foreach ($st in $stars) {
        $r = [Math]::Max(0.6, $st[2] * $S)
        $g.FillEllipse($starBrush, [float]($st[0] * $S - $r), [float]($st[1] * $S - $r), [float]($r * 2), [float]($r * 2))
    }

    # 하얀 행성 고리를 -20도 기울여 그린다.
    # 연분홍 배경에서도 보이도록 바깥에 얇은 분홍 테두리를 먼저 그리고 그 위에 흰 고리를 겹친다.
    $c = $S / 2.0
    $ringW = 0.76 * $S; $ringH = 0.32 * $S
    $ringWidth = [Math]::Max(1.5, 0.075 * $S)
    $outlinePen = New-Object System.Drawing.Pen (Hex "#EE8FB0"), ([float]($ringWidth + [Math]::Max(1, 0.025 * $S)))
    $ringPen = New-Object System.Drawing.Pen (Hex "#FFFFFF"), ([float]$ringWidth)
    $ringRect = New-Object System.Drawing.RectangleF ([float](-$ringW / 2)), ([float](-$ringH / 2)), ([float]$ringW), ([float]$ringH)
    $g.TranslateTransform([float]$c, [float]$c)
    $g.RotateTransform(-20)
    $g.DrawEllipse($outlinePen, $ringRect)
    $g.DrawEllipse($ringPen, $ringRect)
    $g.ResetTransform()

    $g.Dispose()
    return $bmp
}

function MakeLogo([int]$S) {
    $scene = DrawScene $S
    $clear = GetPixels $scene
    $stride = $S * 4
    $out = New-Object byte[] $clear.Length

    # 왼쪽 절반용 노이즈 이미지: 큰 블록으로 뭉갠 뒤 블록마다 밝기 잡음을 섞는다
    $block = [Math]::Max(1, [int][Math]::Floor($S / 18))
    $rand = New-Object System.Random (1000 + $S)
    $blocks = [Math]::Ceiling($S / $block)
    $noise = New-Object int[] ($blocks * $blocks)
    # 밝은 배경이라 어두운 쪽으로 더 흔들어야 노이즈가 보이고, 너무 세면 고리가 묻힌다
    for ($i = 0; $i -lt $noise.Length; $i++) { $noise[$i] = $rand.Next(-26, 9) }

    # 경계선: 중심을 지나고 세로에서 30도 기울어진 선 (위쪽이 오른쪽으로)
    $angle = 30 * [Math]::PI / 180
    $dx = [Math]::Sin($angle); $dy = -[Math]::Cos($angle)
    $c = $S / 2.0

    for ($y = 0; $y -lt $S; $y++) {
        for ($x = 0; $x -lt $S; $x++) {
            $i = $y * $stride + $x * 4
            $px = $x + 0.5 - $c; $py = $y + 0.5 - $c
            $side = $dx * $py - $dy * $px   # 양수면 선의 오른쪽
            if ($side -ge 0) {
                $out[$i] = $clear[$i]; $out[$i + 1] = $clear[$i + 1]; $out[$i + 2] = $clear[$i + 2]
            } else {
                $bx = [int][Math]::Floor($x / $block); $by = [int][Math]::Floor($y / $block)
                $sx = [Math]::Min($S - 1, $bx * $block + [int]($block / 2))
                $sy = [Math]::Min($S - 1, $by * $block + [int]($block / 2))
                $j = $sy * $stride + $sx * 4
                $n = $noise[$by * $blocks + $bx]
                for ($k = 0; $k -lt 3; $k++) {
                    $v = $clear[$j + $k] + $n + $rand.Next(-3, 4)
                    $out[$i + $k] = [byte][Math]::Max(0, [Math]::Min(255, $v))
                }
            }
            $out[$i + 3] = 255
        }
    }
    SetPixels $scene $out

    # 진한 분홍색 사선
    $g = [System.Drawing.Graphics]::FromImage($scene)
    $g.SmoothingMode = "AntiAlias"
    $linePen = New-Object System.Drawing.Pen (Hex "#D81B60"), ([float][Math]::Max(2, 0.07 * $S))
    $len = 0.75 * $S
    $g.DrawLine($linePen, [float]($c - $dx * $len), [float]($c - $dy * $len), [float]($c + $dx * $len), [float]($c + $dy * $len))
    $g.Dispose()

    # 둥근 사각형 바깥은 투명하게
    $mask = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $mg = [System.Drawing.Graphics]::FromImage($mask)
    $mg.SmoothingMode = "AntiAlias"
    $mg.FillPath([System.Drawing.Brushes]::White, (RoundedRect 0 0 ($S - 0.5) ($S - 0.5) (0.22 * $S)))
    $mg.Dispose()
    $m = GetPixels $mask
    $px = GetPixels $scene
    for ($i = 3; $i -lt $px.Length; $i += 4) { $px[$i] = $m[$i] }
    SetPixels $scene $px
    $mask.Dispose()
    return $scene
}

# README용 큰 로고
$logo = MakeLogo 512
$logo.Save((Join-Path $assets "logo.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$logo.Dispose()

# 아이콘 항목 하나: 256px은 PNG, 그보다 작은 크기는 호환성이 좋은 32비트 BMP(DIB)로 담는다
function IconEntry([int]$s) {
    $bmp = MakeLogo $s
    $ms = New-Object System.IO.MemoryStream
    if ($s -ge 256) {
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        $bw = New-Object System.IO.BinaryWriter $ms
        $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
        # BITMAPINFOHEADER (높이는 색 + 마스크라서 2배)
        $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
        $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
        $bw.Write([uint32]($s * $s * 4 + $maskStride * $s))
        $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
        # 색 데이터 (아래 줄부터, BGRA)
        $px = GetPixels $bmp
        for ($y = $s - 1; $y -ge 0; $y--) { $bw.Write($px, $y * $s * 4, $s * 4) }
        # AND 마스크: 알파를 쓰므로 모두 0
        $bw.Write((New-Object byte[] ($maskStride * $s)))
        $bw.Flush()
    }
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = @()
foreach ($s in $sizes) { $pngs += , (IconEntry $s) }
$fs = [System.IO.File]::Create((Join-Path $assets "openscaler.ico"))
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
"생성 완료: assets/logo.png, assets/openscaler.ico"
