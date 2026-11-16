param(
  [Parameter(Mandatory = $true)][string]$Target,   # 게임 창 제목의 일부 (예: "Minecraft")
  [int]$Seconds = 30,                               # 방식마다 기록할 시간
  [string]$Exe = "",                                # OpenScaler.exe 경로. 비우면 이 스크립트 옆, 그다음 현재 폴더에서 찾는다
  [string]$Out = "",                                # 결과 폴더. 비우면 devicecheck_날짜시각
  [switch]$NoPause                                  # 화질 측정 녹화 전에 묻지 않는다 (자동 실행용)
)
# 다른 기기(고주사율 모니터)에서 프레임 생성을 잴 기록을 모은다. 파이썬 없이 돌아가고, 분석은 나중에 한다.
#   python -I tools/test/fgtiming.py 결과폴더/fg_rife.csv         (실시간 등급)
#   OpenScaler.exe --fg-gaptest 결과폴더/dump 결과폴더/gap         (화질 측정)
#   python -I tools/test/fgquality.py 결과폴더/gap/fg_gaptest.csv  (화질 등급)
# 실행 전에 게임을 창 모드로 켜 두고, 기록하는 동안 화면이 계속 움직이도록 게임을 조작한다.
$ErrorActionPreference = "Stop"
if (-not $Exe) {
  # 릴리스 zip에서는 tools 폴더 안에 있으므로 한 단계 위도 찾는다.
  foreach ($c in @((Join-Path $PSScriptRoot "OpenScaler.exe"), (Join-Path $PSScriptRoot "..\OpenScaler.exe"), (Join-Path (Get-Location) "OpenScaler.exe"))) {
    if (Test-Path $c) { $Exe = (Resolve-Path $c).Path; break }
  }
}
if (-not (Test-Path $Exe)) { throw "OpenScaler.exe를 찾지 못했습니다. -Exe로 경로를 주세요." }
if (Get-Process OpenScaler -ErrorAction SilentlyContinue) { throw "OpenScaler가 이미 실행 중입니다. 끄고 다시 실행하세요 (단축키가 겹칩니다)." }
if (-not (Get-Process | Where-Object { $_.MainWindowTitle -like "*$Target*" })) { throw "제목에 '$Target'이 들어간 창이 없습니다. 게임을 먼저 켜 주세요." }
if (-not $Out) { $Out = "devicecheck_" + (Get-Date -Format "yyyyMMdd_HHmmss") }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# 1. 기기 정보
$info = @()
$info += "날짜: " + (Get-Date -Format "yyyy-MM-dd HH:mm")
$info += "OS: " + (Get-CimInstance Win32_OperatingSystem).Caption
$info += "CPU: " + (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
foreach ($v in Get-CimInstance Win32_VideoController) {
  $info += "GPU: $($v.Name), 주사율 $($v.CurrentRefreshRate)Hz, 화면 $($v.CurrentHorizontalResolution)x$($v.CurrentVerticalResolution)"
}
$info += "게임 창: $Target"
$info | Set-Content -Encoding UTF8 "$Out\device.txt"
$info | ForEach-Object { Write-Host $_ }

function Run-OpenScaler([string]$ArgsText, [int]$Timeout) {
  $p = Start-Process $Exe -ArgumentList $ArgsText -PassThru
  if (-not $p.WaitForExit($Timeout * 1000)) { $p.Kill(); Write-Host "  시간이 지나 강제로 끝냈습니다" }
}

function Countdown([string]$Message) {
  Write-Host ""
  Write-Host $Message -ForegroundColor Yellow
  for ($i = 5; $i -ge 1; $i--) { Write-Host "  $i..."; Start-Sleep 1 }
}

# 2. RIFE 신경망 속도 (게임과 상관없이 잰다)
Write-Host "`n[1/5] RIFE 신경망 속도"
Run-OpenScaler "--no-gui --rife-bench `"$Out\bench`"" 300
if (Test-Path "$Out\bench\rife_bench.csv") { Get-Content "$Out\bench\rife_bench.csv" | Write-Host }

# 3. 방식마다 실시간 기록
$modes = @(
  @{ Name = "rife";  Flag = "--fg-rife";        Label = "RIFE" },
  @{ Name = "block"; Flag = "--fg-block";       Label = "블록 매칭" },
  @{ Name = "ext";   Flag = "--fg-extrapolate"; Label = "외삽" }
)
$step = 2
foreach ($m in $modes) {
  Countdown "[$step/5] $($m.Label): 게임 창을 눌러 두고 $Seconds 초 동안 화면이 움직이도록 조작하세요"
  Run-OpenScaler "--no-gui --target `"$Target`" --fg $($m.Flag) --fg-log `"$Out\fg_$($m.Name).csv`" --duration $Seconds" ($Seconds + 30)
  $lines = if (Test-Path "$Out\fg_$($m.Name).csv") { (Get-Content "$Out\fg_$($m.Name).csv").Count } else { 0 }
  Write-Host "  기록 $lines 줄"
  if ($lines -eq 0) { Write-Host "  기록이 비었습니다. 게임 창이 최소화되었거나 닫히지 않았는지 확인하세요." -ForegroundColor Red }
  $step++
}

# 4. 화질 측정용 슬로모션 덤프 (RIFE, 2초). 덤프의 게임 프레임으로 나중에 --fg-gaptest를 돌린다.
# 게임 fps를 제한한 채 녹화하면 이웃한 프레임 간격이 벌어져 촘촘한 간격을 잴 수 없으므로, 녹화 전에 제한을 풀게 한다.
if (-not $NoPause) {
  Write-Host ""
  Write-Host "[5/5] 화질 측정 녹화 전에 게임의 최대 fps 제한을 풀어 주세요 (가능하면 주사율 이상). 준비되면 Enter" -ForegroundColor Yellow
  Read-Host | Out-Null
}
Countdown "[5/5] 화질 측정 녹화: 5초 뒤부터 2초 동안 녹화합니다. 화면이 움직이도록 계속 조작하세요"
Run-OpenScaler "--no-gui --target `"$Target`" --fg --fg-rife --slowmo-auto 5 --slowmo-seconds 2 --slowmo-dump `"$Out\dump`" --duration 40" 70

Write-Host "`n끝났습니다. 결과 폴더: $Out" -ForegroundColor Green
Write-Host "이 폴더를 통째로 가져오면 분석할 수 있습니다."
