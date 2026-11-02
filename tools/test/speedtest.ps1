# 커서 속도 보정: 확대 화면 안에서 속도가 낮아지고, 끝나면 복구되고, 비정상 종료 후에도 복구되는지 확인
$sp = Split-Path $MyInvocation.MyCommand.Path
$dir = "$sp\speedtest"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\dev\OpenScaler\build\OpenScaler.exe" $dir
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class S {
 [DllImport("user32.dll")] public static extern bool SystemParametersInfo(int a, int b, ref int v, int f);
 [DllImport("user32.dll", EntryPoint="SystemParametersInfo")] public static extern bool SetSpeed(int a, int b, IntPtr v, int f);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, int f, int e);
 public static int Get() { int v = 0; SystemParametersInfo(0x70, 0, ref v, 0); return v; }
 public static void Set(int v) { SetSpeed(0x71, 0, new IntPtr(v), 0); }
}
"@
[S]::SetProcessDPIAware() | Out-Null
$orig = [S]::Get()
"original mouse speed: $orig"
$form = $null
try {
  $form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -File `"$sp\patternform.ps1`" -Seconds 40" -PassThru
  Start-Sleep 4
  $h = [S]::FindWindow($null, "OSPattern")
  function Focus { [S]::keybd_event(0x12,0,0,0); [S]::keybd_event(0x12,0,2,0); [S]::SetForegroundWindow($h) | Out-Null }

  # 1) 정상 동작: 확대 화면 안 -> 속도 낮아짐, 종료 후 -> 복구
  [S]::SetCursorPos(5, 1075) | Out-Null
  $os = Start-Process "$dir\OpenScaler.exe" -ArgumentList "--target OSPattern --duration 4 --cursor-speed" -RedirectStandardOutput "$dir\run1.txt" -PassThru -NoNewWindow
  Start-Sleep 1.5; Focus; Start-Sleep 0.3
  [S]::SetCursorPos(960, 540) | Out-Null; Start-Sleep 0.5
  "1) inside scaled view (3x): speed = $([S]::Get())   ini SavedMouseSpeed = $((Get-Content "$dir\OpenScaler.ini" -ErrorAction SilentlyContinue) -join ' ')"
  $os.WaitForExit(10000) | Out-Null
  "1) after exit: speed = $([S]::Get())   ini = $((Get-Content "$dir\OpenScaler.ini" -ErrorAction SilentlyContinue) -join ' ')"

  # 2) 비정상 종료: 속도가 낮아진 상태에서 강제 종료 -> 다음 실행 때 복구
  $os = Start-Process "$dir\OpenScaler.exe" -ArgumentList "--target OSPattern --duration 20 --cursor-speed" -RedirectStandardOutput "$dir\run2.txt" -PassThru -NoNewWindow
  Start-Sleep 1.5; Focus; Start-Sleep 0.3
  [S]::SetCursorPos(960, 540) | Out-Null; Start-Sleep 0.5
  "2) before kill: speed = $([S]::Get())"
  $os.Kill(); Start-Sleep 0.5
  "2) after kill: speed = $([S]::Get())   ini = $((Get-Content "$dir\OpenScaler.ini") -join ' ')"
  $os = Start-Process "$dir\OpenScaler.exe" -ArgumentList "--duration 1" -RedirectStandardOutput "$dir\run3.txt" -PassThru -NoNewWindow
  $os.WaitForExit(10000) | Out-Null
  "2) after restart: speed = $([S]::Get())   ini = $((Get-Content "$dir\OpenScaler.ini") -join ' ')"
  "   restart log: " + ((Get-Content "$dir\run3.txt" -Encoding UTF8 | Where-Object { $_ -match 'SavedMouse|\(\d+\)' }) -join ' ')
}
finally {
  if ([S]::Get() -ne $orig) { [S]::Set($orig); "!! restored original speed $orig manually" }
  if ($form -and -not $form.HasExited) { $form.Kill() }
  Get-Process OpenScaler -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" } | Stop-Process -Force
  "final mouse speed: $([S]::Get())"
}
