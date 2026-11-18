param(
  [Parameter(Mandatory = $true)][string]$Target,   # 게임 창 제목의 일부 (예: "Minecraft")
  [int]$Seconds = 30,                               # 방식마다 기록할 시간
  [string]$Exe = "",                                # OpenScaler.exe 경로. 비우면 이 스크립트 옆, 그다음 현재 폴더에서 찾는다
  [string]$Out = "",                                # 결과 폴더. 비우면 devicecheck_날짜시각
  [string]$Region = "0.25,0,0.5,1",                 # 영역 비교에 쓸 프레임 생성 영역 (x,y,너비,높이 비율). 기본은 가운데 세로 절반
  [switch]$NoPause                                  # 묻지 않고 넘어간다 (자동 실행용)
)
# 다른 기기(고주사율 모니터)에서 프레임 생성을 잴 기록을 모은다. 파이썬 없이 돌아가고, 분석은 나중에 한다.
#   python -I tools/test/fgtiming.py 결과폴더/fg_rife.csv         (실시간 등급)
#   OpenScaler.exe --fg-gaptest 결과폴더/dump 결과폴더/gap         (화질 측정)
#   python -I tools/test/fgquality.py 결과폴더/gap/fg_gaptest.csv  (화질 등급)
# 실행 전에 게임을 창 모드로 켜 두고, 기록하는 동안 화면이 계속 움직이도록 게임을 조작한다.
# 단계: 기기 정보 -> RIFE 속도 -> RIFE, 블록 매칭, 외삽, 블록 매칭+영역 (게임 fps = 주사율 절반)
#       -> 블록 매칭 (게임 fps = 주사율의 0.4배, 정수배가 아닌 배수) -> 화질 측정 덤프 -> 영역 계산 시간 비교 -> 학습 데이터 복사
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

function Ask([string]$Message) {
  if ($NoPause) { return }
  Write-Host ""
  Write-Host $Message -ForegroundColor Yellow
  Read-Host | Out-Null
}

function Record([string]$Name, [string]$Flags, [string]$Label, [string]$Step) {
  Countdown "[$Step] $($Label): 게임 창을 눌러 두고 $Seconds 초 동안 화면이 움직이도록 조작하세요"
  Run-OpenScaler "--no-gui --target `"$Target`" --fg $Flags --fg-log `"$Out\fg_$Name.csv`" --duration $Seconds" ($Seconds + 30)
  $lines = if (Test-Path "$Out\fg_$Name.csv") { (Get-Content "$Out\fg_$Name.csv").Count } else { 0 }
  Write-Host "  기록 $lines 줄"
  if ($lines -eq 0) { Write-Host "  기록이 비었습니다. 게임 창이 최소화되었거나 닫히지 않았는지 확인하세요." -ForegroundColor Red }
}

function Countdown([string]$Message) {
  Write-Host ""
  Write-Host $Message -ForegroundColor Yellow
  for ($i = 5; $i -ge 1; $i--) { Write-Host "  $i..."; Start-Sleep 1 }
}

# 2. RIFE 신경망 속도 (게임과 상관없이 잰다)
$hz = (Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentRefreshRate -gt 1 } | Measure-Object CurrentRefreshRate -Maximum).Maximum
if (-not $hz) { $hz = 60 }
Write-Host "`n[1/7] RIFE 신경망 속도"
Run-OpenScaler "--no-gui --rife-bench `"$Out\bench`"" 300
if (Test-Path "$Out\bench\rife_bench.csv") { Get-Content "$Out\bench\rife_bench.csv" | Write-Host }

# 3. 방식마다 실시간 기록. 프로필에 영역이 있어도 화면 전체로 잰 뒤, 영역을 지정해 한 번 더 잰다 (계산 시간 비교).
Ask "[2/7] 게임의 최대 fps를 주사율의 절반($([int]($hz / 2))fps 정도)으로 맞춰 주세요. 준비되면 Enter"
Record "rife" "--fg-rife --fg-region none" "RIFE" "2/7"
Record "block" "--fg-block --fg-region none" "블록 매칭" "3/7"
Record "ext" "--fg-extrapolate --fg-region none" "외삽" "4/7"
Record "region" "--fg-block --fg-region $Region" "블록 매칭 + 영역 ($Region)" "5/7"

# 4. 정수배가 아닌 배수: 게임 프레임 사이에 넣는 장수가 번갈아 바뀌어도 화면 속 시간이 고르게 흐르는지 본다.
Ask "[6/7] 게임의 최대 fps를 주사율의 0.4배($([int]($hz * 0.4))fps 정도)로 바꿔 주세요. 준비되면 Enter"
Record "block_nonint" "--fg-block --fg-region none" "블록 매칭 (정수배가 아닌 배수)" "6/7"

# 5. 화질 측정용 슬로모션 덤프 (RIFE, 2초). 덤프의 게임 프레임으로 나중에 --fg-gaptest를 돌린다.
# 게임 fps를 제한한 채 녹화하면 이웃한 프레임 간격이 벌어져 촘촘한 간격을 잴 수 없으므로, 녹화 전에 제한을 풀게 한다.
Ask "[7/7] 화질 측정 녹화 전에 게임의 최대 fps 제한을 풀어 주세요 (가능하면 주사율 이상). 준비되면 Enter"
Countdown "[7/7] 화질 측정 녹화: 5초 뒤부터 2초 동안 녹화합니다. 화면이 움직이도록 계속 조작하세요"
Run-OpenScaler "--no-gui --target `"$Target`" --fg --fg-rife --fg-region none --slowmo-auto 5 --slowmo-seconds 2 --slowmo-dump `"$Out\dump`" --duration 40" 70

# 6. 영역 계산 시간 비교: 덤프의 게임 프레임을 화면 전체와 영역으로 번갈아 처리한다 (게임 조작 필요 없음).
# 실시간 기록의 GPU 시간은 게임 화면과 클럭에 따라 흔들리므로, 영역 효과는 이 결과로 판단한다.
Write-Host "`n영역 계산 시간 비교 (게임을 조작하지 않아도 됩니다)"
Run-OpenScaler "--no-gui --fg-regionbench `"$Out\dump`" `"$Out\regionbench`" $Region" 300
if (Test-Path "$Out\regionbench\fg_regionbench_summary.csv") { Get-Content "$Out\regionbench\fg_regionbench_summary.csv" | Write-Host }

# 7. 이 기기에서 쌓인 학습 데이터를 함께 가져간다 (공유 후보와 학습 경과 확인용).
$profiles = Join-Path (Split-Path $Exe) "profiles"
if (Test-Path $profiles) {
  New-Item -ItemType Directory -Force "$Out\profiles" | Out-Null
  Copy-Item "$profiles\*.json" "$Out\profiles" -ErrorAction SilentlyContinue
  Write-Host "학습 데이터 $((Get-ChildItem "$Out\profiles" -Filter *.json).Count)개를 복사했습니다."
}

Write-Host "`n끝났습니다. 결과 폴더: $Out" -ForegroundColor Green
Write-Host "이 폴더를 통째로 가져오면 분석할 수 있습니다."
