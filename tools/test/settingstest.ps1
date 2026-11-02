# 단축키로 설정을 바꾸면 ini에 저장되고, 다시 실행하면 불러오는지 확인한다.
$sp = Split-Path $MyInvocation.MyCommand.Path
$dir = "$sp\settingstest"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\dev\OpenScaler\build\OpenScaler.exe" $dir
Add-Type @"
using System.Runtime.InteropServices;
public static class K {
 [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, int f, int e);
 public static void Hotkey(byte vk) {
  keybd_event(0x12,0,0,0); keybd_event(0x10,0,0,0); keybd_event(vk,0,0,0);
  keybd_event(vk,0,2,0); keybd_event(0x10,0,2,0); keybd_event(0x12,0,2,0);
 }
}
"@
$form = Start-Process powershell -ArgumentList "-NoProfile -WindowStyle Hidden -File `"$sp\patternform.ps1`" -Seconds 14" -PassThru
Start-Sleep 4
$os = Start-Process "$dir\OpenScaler.exe" -ArgumentList "--target OSPattern --duration 5" -RedirectStandardOutput "$dir\run1.txt" -PassThru -NoNewWindow
Start-Sleep 2
[K]::Hotkey(0x4D)   # Alt+Shift+M : FSR 1.0 -> Nearest
Start-Sleep -Milliseconds 300
[K]::Hotkey(0x26)   # Alt+Shift+Up : 샤픈 0.5 -> 0.6
Start-Sleep -Milliseconds 300
[K]::Hotkey(0x49)   # Alt+Shift+I : 정수배 ON
$os.WaitForExit(10000) | Out-Null
$form.Kill()
"--- run1 (hotkeys) ---"
Get-Content "$dir\run1.txt" -Encoding UTF8 | Select-String "경고|현재 설정"
(Get-Content "$dir\run1.txt" -Encoding UTF8 | Select-String "->" | Select-Object -Last 1).Line
"--- OpenScaler.ini ---"
Get-Content "$dir\OpenScaler.ini"
$os2 = Start-Process "$dir\OpenScaler.exe" -ArgumentList "--duration 1" -RedirectStandardOutput "$dir\run2.txt" -PassThru -NoNewWindow
$os2.WaitForExit(10000) | Out-Null
"--- run2 (restart) ---"
Get-Content "$dir\run2.txt" -Encoding UTF8 | Select-String "현재 설정"
