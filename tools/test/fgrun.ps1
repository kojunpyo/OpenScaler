param([string]$OutDir, [int]$Fps = 30, [string]$Extra = "")
# 실제 캡처 경로로 프레임 보간을 실행해 시점 기록과 화면 사진을 남긴다. 몇 초 동안 전체화면 오버레이가 뜬다.
$sp = Split-Path $MyInvocation.MyCommand.Path
Add-Type @"
using System.Runtime.InteropServices;
public static class D2 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[D2]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
$exe = "C:\dev\OpenScaler\build\OpenScaler.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$sp\fgform.ps1`" -Fps $Fps -Seconds 16" -PassThru
Start-Sleep 3
$log = "$OutDir\fg_timing_$Fps.csv"
$args2 = "--no-gui --target OSFrameGen --duration 10 --fg --fg-log `"$log`" $Extra"
$os = Start-Process $exe -ArgumentList $args2 -RedirectStandardOutput "$OutDir\fg_stdout_$Fps.txt" -PassThru -NoNewWindow
Start-Sleep 7
$bmp = New-Object System.Drawing.Bitmap 1920, 1080
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $g.Dispose()
$bmp.Save("$OutDir\fg_screen_$Fps.png")
$bmp.Dispose()
$os.WaitForExit(15000) | Out-Null
$form.Kill()
# 마지막 상태 줄
(Get-Content "$OutDir\fg_stdout_$Fps.txt" -Encoding UTF8 -Raw) -split "`r" | Select-Object -Last 1
