param([int]$W = 640, [int]$H = 360, [int]$Fps = 30, [int]$Seconds = 20)
# 프레임 보간 실행 테스트용 창: 정해진 fps로 무늬와 막대가 움직인다.
Add-Type @"
using System.Runtime.InteropServices;
public static class D { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v); }
"@
[D]::SetProcessDpiAwarenessContext([System.IntPtr]::new(-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$f = New-Object System.Windows.Forms.Form
$f.Text = "OSFrameGen"
$f.StartPosition = "Manual"
$f.Location = New-Object System.Drawing.Point(40, 40)
$f.ClientSize = New-Object System.Drawing.Size($W, $H)
$prop = [System.Windows.Forms.Control].GetProperty("DoubleBuffered", [System.Reflection.BindingFlags]"NonPublic,Instance")
$prop.SetValue($f, $true, $null)

$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 80, 110)), 3
$script:n = 0
$f.Add_Paint({
  param($s, $e)
  $g = $e.Graphics
  $g.Clear([System.Drawing.Color]::FromArgb(24, 26, 38))
  $off = ($script:n * 6) % 80
  for ($x = -80 + $off; $x -lt $W; $x += 80) { $g.DrawLine($pen, $x, 0, $x + 40, $H) }
  $bx = ($script:n * 12) % ($W + 80) - 80
  $g.FillRectangle([System.Drawing.Brushes]::HotPink, $bx, 120, 80, 50)
  $by = ($script:n * 9) % ($H + 40) - 40
  $g.FillRectangle([System.Drawing.Brushes]::SkyBlue, 460, $by, 90, 16)
  $g.FillRectangle([System.Drawing.Brushes]::White, 20, $H - 40, 180, 20)
})

$t = New-Object System.Windows.Forms.Timer
$t.Interval = [int](1000 / $Fps)
$t.Add_Tick({ $script:n++; $f.Invalidate() })
$t.Start()
$stop = New-Object System.Windows.Forms.Timer
$stop.Interval = $Seconds * 1000
$stop.Add_Tick({ $f.Close() })
$stop.Start()
[System.Windows.Forms.Application]::Run($f)
