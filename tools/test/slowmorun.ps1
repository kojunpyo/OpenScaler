param([string]$OutDir, [int]$Fps = 60, [int]$Target = 120, [string]$Extra = "")
# 슬로모션 테스트를 실제 캡처 경로로 실행해 재생 기록, 만든 프레임(BMP), 화면 사진을 남긴다. 몇 초 동안 전체화면 오버레이가 뜬다.
$sp = Split-Path $MyInvocation.MyCommand.Path
Add-Type @"
using System.Runtime.InteropServices;
public static class D3 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[D3]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
$exe = "C:\dev\OpenScaler\build\OpenScaler.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$sp\fgform.ps1`" -Fps $Fps -Seconds 22" -PassThru
Start-Sleep 3
$log = "$OutDir\slowmo.csv"
$args2 = "--no-gui --target OSFrameGen --duration 14 --slowmo-auto 3 --slowmo-target $Target --slowmo-dump `"$OutDir\frames`" --fg-log `"$log`" $Extra"
$os = Start-Process $exe -ArgumentList $args2 -RedirectStandardOutput "$OutDir\stdout.txt" -PassThru -NoNewWindow
Start-Sleep 10
$bmp = New-Object System.Drawing.Bitmap 1920, 1080
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $g.Dispose()
$bmp.Save("$OutDir\screen.png")
$bmp.Dispose()
$os.WaitForExit(20000) | Out-Null
$form.Kill()

# 재생 기록 분석: 한 바퀴 동안 몇 장을 몇 초에 보여 줬는지, 건너뛴 프레임이 있는지 본다.
$rows = Get-Content $log | Where-Object { $_ -like "slowmo,*" } | ForEach-Object {
    $a = $_ -split ","
    [pscustomobject]@{ Time = [double]$a[1]; Index = [int]$a[2]; Real = [int]$a[3]; T = [double]$a[4]; Gen = [int]$a[5] }
}
$starts = @(); for ($i = 0; $i -lt $rows.Count; $i++) { if ($rows[$i].Index -eq 0 -and ($i -eq 0 -or $rows[$i - 1].Index -ne 0)) { $starts += $i } }
if ($starts.Count -lt 2) { "한 바퀴를 다 돌지 못했습니다. 기록 $($rows.Count)줄"; exit 1 }
$lap = $rows[$starts[0]..($starts[1] - 1)]
$distinct = @($lap | Select-Object -ExpandProperty Index -Unique)
$last = $distinct[-1]
$lastRow = ($lap | Where-Object { $_.Index -eq $last })[0]
$skipped = 0; for ($i = 1; $i -lt $distinct.Count; $i++) { if ($distinct[$i] -ne $distinct[$i - 1] + 1) { $skipped++ } }
$gen = @($lap | Where-Object { $_.Index -ne $last -or $_ -eq $lastRow } | Where-Object { $_.Gen -eq 1 }).Count
"재생 프레임 $($distinct.Count)장 (진짜 $($distinct.Count - $gen), 생성 $gen), 건너뜀 $skipped"
"한 바퀴 재생 시간 {0:N2}초 (끝 멈춤 제외), 주사율로 나눈 값과 비교" -f ($lastRow.Time - $lap[0].Time)
"bmp 파일 $((Get-ChildItem "$OutDir\frames" -Filter *.bmp).Count)개"
(Get-Content "$OutDir\stdout.txt" -Encoding UTF8) | Where-Object { $_ -match "슬로모션" }
