param([string]$Out, [int]$X = 560, [int]$Y = 560, [int]$W = 800, [int]$H = 300)
# 비교 모드로 확대한 화면을 찍어 가운데 구분선 주변을 잘라 저장한다.
$sp = Split-Path $MyInvocation.MyCommand.Path
Add-Type @"
using System.Runtime.InteropServices;
public static class D3 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[D3]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
$form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -File `"$sp\patternform.ps1`" -Seconds 14" -PassThru
Start-Sleep 4
$os = Start-Process "C:\dev\OpenScaler\build\OpenScaler.exe" -ArgumentList "--target OSPattern --duration 7 --compare" -RedirectStandardOutput "$env:TEMP\os_split.txt" -PassThru -NoNewWindow
Start-Sleep 4.5
$bmp = New-Object System.Drawing.Bitmap 1920, 1080
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $g.Dispose()
$os.WaitForExit(10000) | Out-Null; $form.Kill()
$crop = $bmp.Clone((New-Object System.Drawing.Rectangle $X, $Y, $W, $H), $bmp.PixelFormat)
$crop.Save($Out)
"divider pixel at (959,700): " + $bmp.GetPixel(959, 700)
(Get-Content "$env:TEMP\os_split.txt" -Encoding UTF8 | Select-String "->" | Select-Object -Last 1).Line
$crop.Dispose(); $bmp.Dispose()
