param([string]$Dir, [string]$Scene, [string]$OutFile)
# 자체 테스트가 저장한 BMP를 정답/움직임 보간/블렌딩/움직임 표시 순서로 묶어 PNG로 만든다.
Add-Type -AssemblyName System.Drawing
$names = "truth", "motion", "blend", "flow"
$imgs = $names | ForEach-Object { [System.Drawing.Image]::FromFile("$Dir\${Scene}_$_.bmp") }
$w = $imgs[0].Width; $h = $imgs[0].Height
$canvas = New-Object System.Drawing.Bitmap ($w * 2 + 10), ($h * 2 + 10)
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font "Segoe UI", 16, ([System.Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt 4; $i++) {
  $x = ($i % 2) * ($w + 10); $y = [math]::Floor($i / 2) * ($h + 10)
  $g.DrawImage($imgs[$i], $x, $y, $w, $h)
  $g.DrawString($names[$i], $font, [System.Drawing.Brushes]::Yellow, $x + 8, $y + 8)
}
$g.Dispose()
$canvas.Save($OutFile)
$imgs | ForEach-Object { $_.Dispose() }
