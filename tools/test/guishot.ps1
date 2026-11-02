param([string]$Out, [string]$ExeArgs = "", [int]$Wait = 3, [switch]$MoveToSecondMonitor, [int]$Wheel = 0)
# 설정 창을 띄우고 창 영역만 캡처한 뒤 닫는다.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class G {
 [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
 [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, int d, int e);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int ht, uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, int f, int e);
 [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out R r, int s);
}
"@
[G]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
$exe = "C:\dev\OpenScaler\build\OpenScaler.exe"; $p = if ($ExeArgs) { Start-Process $exe -ArgumentList $ExeArgs -PassThru } else { Start-Process $exe -PassThru }
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [G]::FindWindow("OpenScalerSettings", "OpenScaler") }
if ($h -eq [IntPtr]::Zero) { "settings window not found"; $p.Kill(); exit 1 }
if ($MoveToSecondMonitor) { [G]::SetWindowPos($h, [IntPtr]::Zero, 2100, 100, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010) | Out-Null }
else { [G]::keybd_event(0x12,0,0,0); [G]::keybd_event(0x12,0,2,0); [G]::SetForegroundWindow($h) | Out-Null }
Start-Sleep $Wait
if ($Wheel -ne 0) {
  # 마우스를 창 위로 잠깐 옮겨 아래로 스크롤하고 원래 자리로 돌려 놓는다.
  $before = New-Object G+P
  [G]::GetCursorPos([ref]$before) | Out-Null
  $w0 = New-Object G+R
  [G]::DwmGetWindowAttribute($h, 9, [ref]$w0, 16) | Out-Null
  [G]::SetCursorPos($w0.l + 200, $w0.t + 400) | Out-Null
  Start-Sleep -Milliseconds 300
  for ($i = 0; $i -lt $Wheel; $i++) {
    [G]::mouse_event(0x0800, 0, 0, -120, 0)
    Start-Sleep -Milliseconds 150
  }
  Start-Sleep 1
  [G]::SetCursorPos($before.x, $before.y) | Out-Null
}
$r = New-Object G+R
[G]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) | Out-Null
$w = $r.r - $r.l; $ht = $r.b - $r.t
"window: ($($r.l),$($r.t)) ${w}x${ht}"
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.l, $r.t, 0, 0, $bmp.Size); $g.Dispose()
$bmp.Save($Out); $bmp.Dispose()
[G]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
if (-not $p.WaitForExit(5000)) { "did not exit on close"; $p.Kill() } else { "exit code: $($p.ExitCode)" }
