param([int]$W = 640, [int]$H = 360, [int]$Seconds = 30)
# 업스케일 비교용 테스트 패턴 창 (대각선, 원, 글자)
Add-Type @"
using System.Runtime.InteropServices;
public static class D { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v); }
"@
[D]::SetProcessDpiAwarenessContext([System.IntPtr]::new(-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = "AntiAlias"
$g.TextRenderingHint = "AntiAliasGridFit"
$g.Clear([System.Drawing.Color]::FromArgb(30, 34, 48))
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 1.5
for ($i = 0; $i -lt 12; $i++) { $g.DrawLine($pen, 20, 20 + $i * 8, 200, 60 + $i * 14) }
$g.DrawEllipse($pen, 240, 30, 120, 120)
$g.DrawEllipse($pen, 270, 60, 60, 60)
$g.FillPolygon([System.Drawing.Brushes]::Orange, @((New-Object System.Drawing.Point 400, 150), (New-Object System.Drawing.Point 470, 30), (New-Object System.Drawing.Point 540, 150)))
$font = New-Object System.Drawing.Font "Segoe UI", 11
$g.DrawString("OpenScaler FSR 1.0 test", $font, [System.Drawing.Brushes]::White, 20, 200)
$g.DrawString("업스케일 화질 비교 0123456789", $font, [System.Drawing.Brushes]::LightGreen, 20, 225)
$small = New-Object System.Drawing.Font "Segoe UI", 8
$g.DrawString("small text: The quick brown fox jumps over the lazy dog", $small, [System.Drawing.Brushes]::White, 20, 260)
for ($x = 0; $x -lt 200; $x += 4) { $g.FillRectangle([System.Drawing.Brushes]::White, 380 + $x, 200, 2, 2 + ($x % 40)) }
$g.Dispose()

$f = New-Object System.Windows.Forms.Form
$f.Text = "OSPattern"
$f.StartPosition = "Manual"
$f.Location = New-Object System.Drawing.Point(40, 40)
$f.ClientSize = New-Object System.Drawing.Size($W, $H)
$pb = New-Object System.Windows.Forms.PictureBox
$pb.Dock = "Fill"
$pb.Image = $bmp
$f.Controls.Add($pb)
$t = New-Object System.Windows.Forms.Timer
$t.Interval = $Seconds * 1000
$t.Add_Tick({ $f.Close() })
$t.Start()
[System.Windows.Forms.Application]::Run($f)
