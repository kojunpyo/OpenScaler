param([int]$W = 480, [int]$H = 270, [int]$Fps = 30, [int]$Seconds = 40)
# README 시연 화면용 창: 왼쪽과 오른쪽 절반에 같은 장면을 그린다. 위쪽 30픽셀은 슬로모션 글자 자리로 비워 둔다.
# 비교 모드는 화면 왼쪽 절반을 일반 확대로, 오른쪽 절반을 OpenScaler로 보여 주므로 같은 장면을 나란히 비교할 수 있다.
Add-Type @"
using System.Runtime.InteropServices;
public static class D { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v); }
"@
[D]::SetProcessDpiAwarenessContext([System.IntPtr]::new(-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$f = New-Object System.Windows.Forms.Form
$f.Text = "OSDemo"
$f.StartPosition = "Manual"
$f.Location = New-Object System.Drawing.Point(40, 40)
$f.ClientSize = New-Object System.Drawing.Size($W, $H)
$prop = [System.Windows.Forms.Control].GetProperty("DoubleBuffered", [System.Reflection.BindingFlags]"NonPublic,Instance")
$prop.SetValue($f, $true, $null)

$heart = @(
  "0110110",
  "1111111",
  "1111111",
  "0111110",
  "0011100",
  "0001000"
)
$bg = [System.Drawing.Color]::FromArgb(28, 24, 40)
$lanePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 64, 100)), 1
$pink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 120, 170))
$sky = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(130, 200, 255))
$font = New-Object System.Drawing.Font("Segoe UI", 9)
$small = New-Object System.Drawing.Font("Segoe UI", 7)
$script:n = 0

$f.Add_Paint({
  param($s, $e)
  $g = $e.Graphics
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
  $g.Clear($bg)
  $half = [int]($W / 2)
  foreach ($ox in 0, $half) {
    # 글자: 확대했을 때 흐림과 선명함 차이가 잘 보인다
    $g.DrawString("OpenScaler", $font, [System.Drawing.Brushes]::White, $ox + 10, 40)
    $g.DrawString("480x270 -> 1920x1080", $small, [System.Drawing.Brushes]::Gainsboro, $ox + 10, 58)
    # 픽셀 하트
    for ($y = 0; $y -lt $heart.Count; $y++) {
      for ($x = 0; $x -lt 7; $x++) {
        if ($heart[$y][$x] -eq '1') { $g.FillRectangle($pink, $ox + 190 + $x * 3, 44 + $y * 3, 3, 3) }
      }
    }
    # 리듬게임처럼 떨어지는 노트 (세 줄, 속도가 다르다)
    for ($l = 0; $l -lt 3; $l++) {
      $lx = $ox + 150 + $l * 26
      $g.DrawLine($lanePen, $lx, 80, $lx, $H - 30)
      $g.DrawLine($lanePen, $lx + 22, 80, $lx + 22, $H - 30)
      $ny = 80 + (($script:n * (5 + $l * 2) + $l * 50) % ($H - 116))
      $g.FillRectangle($sky, $lx + 2, $ny, 19, 6)
    }
    $g.FillRectangle([System.Drawing.Brushes]::White, $ox + 150, $H - 32, 74, 2)
    # 좌우로 오가는 상자
    $t = ($script:n * 4) % 220
    $bx = if ($t -lt 110) { $t } else { 220 - $t }
    $g.FillRectangle($pink, $ox + 10 + $bx, 130, 24, 24)
    # 기울어진 줄무늬가 흘러간다
    $off = ($script:n * 3) % 20
    for ($x = -20 + $off; $x -lt 130; $x += 20) { $g.DrawLine($lanePen, $ox + 10 + $x, 180, $ox + 30 + $x, 236) }
  }
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
