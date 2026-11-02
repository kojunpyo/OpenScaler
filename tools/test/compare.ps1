param([string]$OutDir)
# 각 모드로 확대한 화면을 찍고, 같은 영역을 잘라 비교 이미지를 만든다.
$sp = Split-Path $MyInvocation.MyCommand.Path
Add-Type @"
using System.Runtime.InteropServices;
public static class D2 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[D2]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
$exe = "C:\dev\OpenScaler\build\OpenScaler.exe"
$form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -File `"$sp\patternform.ps1`" -Seconds 40" -PassThru
Start-Sleep 4
$modes = @(@(0, "Nearest", 0), @(3, "Lanczos2", 0.5), @(4, "FSR 1.0", 0.5))
$crop = New-Object System.Drawing.Rectangle 0, 0, 0, 0
$shots = @()
foreach ($m in $modes) {
  $os = Start-Process $exe -ArgumentList "--target OSPattern --duration 3 --mode $($m[0]) --sharp $($m[2])" -RedirectStandardOutput "$env:TEMP\os_cmp.txt" -PassThru -NoNewWindow
  Start-Sleep 2
  $bmp = New-Object System.Drawing.Bitmap 1920, 1080
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $g.Dispose()
  $shots += ,@($m[1], $bmp)
  $os.WaitForExit(10000) | Out-Null
  (Get-Content "$env:TEMP\os_cmp.txt" -Encoding UTF8 | Select-String "->" | Select-Object -Last 1).Line
}
$form.Kill()
# 원본 640x360 -> 1920x1080 (3배). 원본 (20,10)-(220,170) 영역 = 화면 (60,30)-(660,510)
$src = New-Object System.Drawing.Rectangle 45, 585, 600, 240
$out = New-Object System.Drawing.Bitmap (600 * 3 + 40), (240 + 40)
$g = [System.Drawing.Graphics]::FromImage($out)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font "Segoe UI", 14, ([System.Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt $shots.Count; $i++) {
  $x = $i * 620
  $g.DrawImage($shots[$i][1], (New-Object System.Drawing.Rectangle $x, 40, 600, 240), $src, [System.Drawing.GraphicsUnit]::Pixel)
  $g.DrawString($shots[$i][0], $font, [System.Drawing.Brushes]::Black, $x + 4, 6)
}
$g.Dispose()
$out.Save("$OutDir\compare_modes.png")
foreach ($s in $shots) { $s[1].Dispose() }
"saved"
