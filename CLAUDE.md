# OpenScaler

창 모드 게임을 실시간 캡처해 전체화면으로 업스케일하는 Windows용 오픈소스 프로그램.
오픈소스소프트웨어 과목 텀프로젝트이며, Lossless Scaling의 기능을 직접 구현하는 것이 목표다.
저장소: https://github.com/kojunpyo/OpenScaler (브랜치 main, 라이선스 MIT)

## 작업 규칙

- 사용자에게는 한국어로 답한다. 코드 주석도 한국어로 쓰고 주변 코드 스타일을 따른다.
- 커밋, 푸시, 태그, 릴리스는 사용자가 요청할 때만 한다. 기능을 만든 뒤 먼저 묻는다.
- 푸시 후에는 GitHub Actions 결과를 확인해서 알려 준다.
- 릴리스는 `v*` 태그를 푸시하면 자동 생성된다. 태그 전에 `CMakeLists.txt`의 버전이 맞는지 확인한다.
- `third_party`의 코드는 수정하지 않는다. 새 외부 코드를 넣기 전에 라이선스를 확인하고 README에 표기한다.
- 사용자 디자인 요구를 임의로 바꾸지 않는다. 덧붙인 부분이 있으면 알려 준다.

## 빌드

```powershell
$vs="C:\Program Files\Microsoft Visual Studio\18\Community"
cmd /c "chcp 949 >nul && `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build 2>&1"
```

- 결과물은 `buildOpenScaler.exe`. 빌드 후 프로젝트 루트의 `OpenScaler.exe`로 복사해 사용자가 쓰게 한다.
- 사용자가 루트 exe를 실행 중이면 복사가 실패한다. 프로세스를 끄지 말고 알린다.
- exe, `build/`, `OpenScaler.ini`, `profiles/`, `rife/`는 git에 올리지 않는다.
- RIFE를 시험하려면 `rife` 폴더를 `build\`에도 두어야 한다 (`build\OpenScaler.exe`는 자기 옆에서 찾는다).
- 릴리스 zip의 `rife` 폴더는 GitHub Actions가 만든다 (NuGet에서 ONNX Runtime 1.24.4, DirectML 1.15.4, Hugging Face 모델을 받아 `tools/rife_export.py`로 변환, 캐시함). 버전을 바꾸면 워크플로의 캐시 키도 바꾼다.
- `chcp 949`는 빼지 않는다. Ninja는 컴파일러의 "참고: 포함 파일:" 줄로 헤더 의존성을 기록하는데, CMake는 이 문구를 시스템 코드 페이지(949)로 저장하고 컴파일러는 콘솔 코드 페이지로 출력한다. Claude가 쓰는 셸은 65001(UTF-8)이라 둘이 어긋나면 의존성이 하나도 기록되지 않는다. 그러면 헤더만 바꿨을 때 다시 컴파일되지 않고, 클래스 멤버를 바꾸면 옛 크기로 컴파일된 파일이 남아 힙 손상으로 죽는다. 확인: `ninja -t deps CMakeFiles/OpenScaler.dir/src/main.cpp.obj`가 `#deps 0`이 아니어야 한다.
- GitHub CLI(`gh`)는 없다. Actions 결과는 공개 API로 확인한다 (`tools/test/waitci.ps1`).

## 구조

- `src/main.cpp`: 캡처(Windows.Graphics.Capture), 오버레이 창, D3D11 렌더링, 커서 좌표 변환과 속도 보정, 설정 파일, 단축키, 메인 루프
- `src/app.h`: main.cpp와 gui.cpp가 함께 쓰는 설정과 기능
- `src/gui.cpp`: 설정 창 (Dear ImGui 1.91.5). 확대 화면 위 메뉴(Alt+Shift+G)도 그린다: ImGui 컨텍스트를 하나 더 두고 글꼴 아틀라스를 함께 쓰며, 설정 구역은 `DrawSettingsBoxes`를 같이 쓴다. 메뉴 입력은 Win32 백엔드 없이 오버레이 창 프로시저가 `GuiOverlayMouse`로 넘긴다
- `src/shaders.h`: 실행 중에 컴파일하는 HLSL (업스케일, 샤프닝, 커서)
- `src/framegen.*`, `src/framegen_shaders.h`: 프레임 생성 (움직임 추정, 합성, 표시 시점, 채점)
- `src/framegen_test.*`: 프레임 생성 자체 테스트 (`--fg-selftest 폴더`, 화면과 마우스를 쓰지 않음). RIFE 속도는 `--rife-bench 폴더`, 영역 결과는 `--fg-regiontest 폴더`, 영역 계산 시간은 `--fg-regionbench 덤프 폴더 [x,y,w,h]`
- `src/game_profile.*`: 게임별 프로필 (설정 자동 조정, 화면 지도, `profiles/*.json`)
- `src/rife.*`: RIFE 실행 (실행 파일 옆 `rife` 폴더의 ONNX Runtime과 모델을 실행 중에 불러옴). `tools/rife_export.py`: 모델 변환
- `src/fsr.hlsl`, `src/fsr_constants.*`: FSR 1.0. fxc로 빌드 때 컴파일
- `third_party/`: fsr(MIT), imgui(MIT), neodgm(Neo둥근모 Pro, OFL 1.1), onnxruntime 헤더(MIT)
- `tools/make_icon.ps1`: 로고와 아이콘 생성
- `tools/profile_info.py`: 학습 데이터 요약, 비교, 형식 검사 (CI가 `shared-profiles/`를 검사). 공유 방법은 `docs/sharing-profiles.md`
- `tools/test/`: 수동 테스트 스크립트 (`pmcompare.py`: PresentMon 기록으로 다른 확대 프로그램과 비교, `demoform.ps1`: README 시연 화면용 창, `devicecheck.ps1`: 다른 기기에서 기록 모으기, `fgtiming.py`/`fgquality.py`: README 평가 기준으로 등급 매기기)

렌더링 순서: 캡처 -> 보이는 클라이언트 영역 복사 -> 프레임 생성(켠 경우) -> Pass 1 업스케일 -> Pass 2 샤프닝 -> 비교 모드 -> 커서 -> Direct2D 상태 표시 -> Present.

## 현재 상태

- 코드 버전 1.5.1, 공개 릴리스 v1.5.1 (2026-11-24: 확대 화면 위 메뉴의 확대 중지로 멈추면 프로그램이 죽던 문제 수정, 영역 지정 뒤 메뉴 다시 열기, README 3.2 보충과 목차 접기). 이전 공개 릴리스 v1.5.0 (2026-11-24: 설정 창 새 디자인. Neo둥근모 Pro 글꼴, 하늘색 테마와 로고, 설명을 (?) 도움말로, 두 열 배치, 100%에서 GitHub Desktop만 한 창 크기, 대상 창 강조). 이전 공개 릴리스 v1.4.4 (2026-11-23: 30fps 게임에서 Lossless Scaling과 부드러움, 화질 비교, 코드 변경 없음). 이전 공개 릴리스 v1.4.3 (2026-11-23: DJMAX에서 영역 팝업 흐름 확인, RIFE 144Hz 끊김을 실행 환경의 한계로 README 8장에 기술, 코드 변경 없음). 이전 공개 릴리스 v1.4.2 (2026-11-23: GPU를 많이 쓰는 게임에서 GPU 우선순위와 RIFE 측정, 코드 변경 없음). 이전 공개 릴리스 v1.4.1 (2026-11-23: 슬로모션 기본값을 모니터 주사율로, RIFE GPU 우선순위 측정 옵션 `--rife-gpu-priority`). 이전 공개 릴리스 v1.3.11 (2026-11-23: Lossless Scaling과 부드러움, 화질 비교, README 6장 재구성과 설명 그림, 코드 변경 없음). 이전 공개 릴리스 v1.3.10 (2026-11-23: 확대 중 GPU 스케줄링 우선순위를 실시간으로, RIFE일 때는 보통). 이전 공개 릴리스 v1.3.9 (2026-11-23: 합성과 움직임 찾기에서 같은 후보를 다시 계산하지 않음, 결과는 같고 DJMAX 144Hz 끊김이 절반 정도로). 이전 공개 릴리스 v1.3.8 (2026-11-22: README 시연 화면을 일반 확대와 OpenScaler를 나란히 붙인 게임 영상으로 교체, 코드 변경 없음). 이전 공개 릴리스 v1.3.7 (2026-11-22: 영역 추천, 확대 화면 위 메뉴 Alt+Shift+G, 확대하면 메뉴와 "프레임 생성 영역을 지정할까요?" 팝업). 이전 공개 릴리스 v1.3.4 (144Hz 끊김 개선: 학습 채점을 나눠 돌림). 고주사율 확인은 2026-11-22에 했다 (주 모니터를 144Hz로, NVIDIA 전원 관리 "최대 성능 선호"가 필요했다). 이전 공개 릴리스 v1.3.2 (2026-11-18, 영역 계산 시간 벤치마크와 README 목차. v1.3.0에서 프레임 생성 영역과 학습 데이터 공유 추가). README 제목에는 번호가 붙어 있으니 앵커 링크는 `#5-게임별-학습`처럼 번호까지 쓴다. 고주사율 확인은 끝났고 남은 개선 후보는 아래 "다음 작업"에 있다.
- 지켜야 할 결정:
  - 프레임 생성 방식은 블록 매칭 보간(기본값) / RIFE 보간 / 블록 매칭 외삽 하나로 고른다 (`FgModeOf`, `SetFgMode`). 게임별 학습이 블록 매칭에만 적용되므로 블록 매칭이 기본값이다.
  - 게임별 학습은 보간용과 외삽용을 따로 둔다 (`GameProfile::interp`, `extrap`, 프로필의 `x_` 값). RIFE는 학습되지 않는다.
  - DJMAX 테스트는 게임에 Enter 외의 입력을 보내지 않는다 (비정상 동작으로 보고 꺼질 수 있다). 사용자가 곡 선택 화면을 준비해 둔다.
  - 학습 데이터 공유 기준은 사용자가 확인했다: 공유 권장은 최근 채점 200회 이상, `shared-profiles` 파일 교체는 같은 방식의 "기준보다 +dB"가 0.2dB 넘게 높을 때 (`docs/sharing-profiles.md`).
  - 확대 중에는 GPU 스케줄링 우선순위를 실시간(`D3DKMTSetProcessSchedulingPriorityClass` 5)으로 올린다 (`UpdateGpuPriority`). RIFE는 신경망이 GPU를 길게 써서 게임과 바탕화면을 밀어낼 수 있으므로 보통으로 둔다 (2026-11-23에 높음, 실시간도 재 봤지만 끊김이 줄지 않았고 실시간은 게임 화면이 빠졌다). GPU를 많이 쓰는 게임에서는 실시간이 게임 fps를 4~6% 낮추지만 확대 화면 끊김을 7분의 1로 줄여 그대로 둔다 (2026-11-23, 수직 동기화 끈 마인크래프트). 같은 게임에서 RIFE를 실시간으로 하면 끊김이 5분의 1이 되고 게임 fps는 7% 줄었지만, 60fps 게임에서는 효과가 없어 RIFE는 보통으로 둔다. 비교 측정은 `--no-gpu-priority`, `--rife-gpu-priority`.
  - RIFE의 144Hz 끊김은 실행 환경(GTX 1060)의 한계로 README 8장에 적고 더 고치지 않는다 (2026-11-23, 사용자 결정).
  - 144Hz 끊김은 화면 기준(PresentMon, `tools/test/pmcompare.py`)으로 적는다. `--fg-log`의 끊김은 화면 합성에서 밀린 장면을 못 봐서 1.5~2배 적게 나온다.
  - 성능은 README 6.1 "평가 기준: 정확성, 즉각성"으로 매긴다 (`tools/test/fgtiming.py`, `fgquality.py`).
  - README는 `docs/`의 개발 문서를 링크하지 않는다. 사용자에게 필요한 내용은 README에 직접 쓰고, 개발 정보는 9장 "개발과 기여"에 둔다. 성능 표에는 현재 버전의 결과만 둔다. 프레임 생성에 "(실험)"을 달지 않는다 (외삽 방식만 실험 표시).
  - 확대를 시작하면 메뉴를 열고 "프레임 생성 영역을 지정할까요?" 팝업을 띄운다 (문구는 사용자가 정함, `kAskRegionTitle`). 설정 창을 쓸 때만이고 `--no-gui` 측정과 시험에서는 띄우지 않는다 (`--region-wizard`로 강제). "예"를 누르면 계산하는 동안 메뉴를 숨겼다가, 영역을 저장하거나 취소하면(좁힐 곳이 없어 닫아도) 메뉴를 다시 연다 (`g_wizardReturnMenu`, 2026-11-24 사용자 요청).
- 다음 작업 (자세한 내용과 측정 기록은 `docs/frame-generation-plan.md`):
  1. 고주사율 확인 결과(`docs/frame-generation-plan.md` "고주사율 확인")에서 남은 것: 화면이 큰 게임(DJMAX 1280x720)은 144Hz에서 화면 전체 프레임 생성이 끊긴다. 1.3.9에서 같은 곡 번갈아 재기로 분당 109~152번 -> 35~92번, 건반 영역 113번 -> 25번 (`docs/frame-generation-plan.md` "144Hz GPU 시간 줄이기"). 남은 끊김은 게임의 GPU 작업과 겹치는 순간에 몰려 났다. 1.3.10에서 GPU 스케줄링 우선순위를 실시간으로 올려 분당 45~108번 -> 10~49번, 건반 영역 0번 ("GPU 스케줄링 우선순위"). 1.3.5에서 영역 추천 버튼을 만들었다 (`FrameGen::RecommendRegion`, 시험 `--region-recommend 초`). 움직임 찾기를 두 화면에 나누는 방법은 효과가 없었다.
  2. 그 결과로 정할 개선 후보: 고정 UI와 조준점 깨짐(화면 지도의 고정 칸 보호), 정수배가 아닌 배수의 화면 간격, 장면 전환 감지, 게임별 학습과 RIFE 연결, 긴 간격에서 밝기가 바뀌는 글자 깨짐.
  3. README: 2026-11-23에 구성을 바꿨다 (설치와 사용법을 3장으로, 문제 해결 3.7, 알려진 한계 8장, 개발과 기여 9장, 성능 표는 현재 버전만). 2026-11-23에 시연 화면을 0.5배속으로, 6장을 "6.1 평가 기준 / 6.2 성능 평가(OpenScaler, Lossless Scaling 비교)"로 바꾸고, 4.2에 프레임 생성 설명 그림(`docs/fg_between.png`), 4.3에 확대 비교 그림(`docs/upscale_compare.png`)을 넣었다. 남은 그림 후보: 영역 추천 흐름, 빠른 시작, 움직임 시각화, 설정 창.
  4. 운영: 이슈/PR 템플릿, CONTRIBUTING.md.
  5. Lossless Scaling 비교는 2026-11-23에 했다 (README 6.3, `docs/frame-generation-plan.md` "Lossless Scaling과 비교"). 부드러움은 PresentMon으로 쟀다. 화질은 OBS와 Lossless Scaling을 모두 관리자 권한으로 실행하고 OBS 게임 캡처로 녹화해 정답 프레임과 비교했다 (DJMAX: OpenScaler 블록 90%, LS 85% / 마인크래프트: LS 87%, RIFE 81%, 블록 46%). Lossless Scaling은 캡처 API가 DXGI면 출력을 화면 캡처에서 뺀다. PresentMon은 관리자 권한이 필요하고 `-WindowStyle Hidden`으로 띄워야 스크립트가 창을 바꿀 수 있다.

## 사용 중인 단축키

Alt+Shift + S(시작/중지), M(모드), I(정수배), Up/Down(샤프닝), C(비교), O(상태 표시), P(커서 속도), F(프레임 생성), T(슬로모션 테스트), G(확대 화면 위 메뉴), Q(종료).
슬로모션 재생 중에만 Space(멈춤), Left/Right(한 장씩)를 등록한다. 이 PC에서 Alt+Shift+R은 다른 프로그램이 이미 쓰고 있어 등록되지 않는다.

## 설정 창 디자인 (사용자 지정)

- 흰 바탕, 각진 모서리, 두꺼운 픽셀 테두리, 언더테일 느낌. 구역 제목은 `* `로 시작하고 픽셀 하트를 쓴다.
- 색은 하늘색 두 가지 (2026-11-24 사용자 지정): 연한 하늘색 `#A6DAF4`는 모든 구역 테두리, 진한 하늘색 `#00AFF0`은 제목과 주요 버튼 같은 중요한 글. 본문과 설명은 이 두 색에서 만든 조금 어두운 하늘색(`kSkyText`, `kSkySoft`)으로 읽히게 한다. 대상 창(확대 화면 위 메뉴의 "확대 중")만 옅은 하늘색 바탕(`#EAF6FD`)과 진한 하늘색 테두리(굵기는 다른 구역과 같음)로 강조하고(`BeginTargetBox`, `kTargetPalette`, 2026-11-24 사용자 선택), 안의 목록 칸과 버튼은 흰 바탕이다. 나머지 구역은 같은 색 묶음(`kSkyPalette`)이다.
- 제목은 Open 연한 하늘색, Scaler 진한 하늘색.
- 글꼴은 Neo둥근모 Pro(`third_party/neodgm`, SIL OFL 1.1) 하나. 글꼴은 16px 기준이지만 사용자 요청(2026-11-24)으로 조금 작게 그린다: 본문과 옵션 이름 14px x P, 덜 중요한 글(설명, 상태, (?)와 도움말) 12px x P (`g_fontThin`), 제목 28px x P. 16px의 정수 배가 아니라 획 굵기가 조금 고르지 않은 것은 감수한다.
- 크기 (2026-11-24 사용자 지정): 창은 100% 배율에서 GitHub Desktop 창(978x668)쯤. 픽셀 단위 P는 Windows 배율에 비례하는 실수다 (`PixelScaleFor`: 100%에서 4/3, 125%에서 5/3). 기본 창은 720 x 484 단위(100%에서 창 974x682).
- 기능 설명은 화면에 길게 쓰지 않고 (?) 도움말(`HelpMarker`, 구역 제목은 `BeginBox`의 help)로 넣는다. 화면에는 상태(fps, 영역, 녹화 진행 등)만 짧게 둔다.
- 배치 (2026-11-24 사용자 지정): 머리말(로고, 제목, 버전 한 줄)과 대상 창은 가로 전체, 그 아래 두 열. 왼쪽 열은 업스케일, 프레임 생성, 단축키. 오른쪽 열은 게임별 학습, 화면 표시 · 마우스, 슬로모션 테스트. 단축키와 슬로모션 테스트는 기본으로 접혀 있고 같은 높이에서 시작한다 (`FoldHeader`, 두 줄 표의 둘째 줄). 프레임 생성과 화면 표시 · 마우스는 아랫단을 맞춘다 (짧은 쪽 상자를 지난 프레임에 잰 긴 쪽 아랫단까지 늘림, `BoxFit`). 설정 자동 저장 문구는 쓰지 않는다. 창이 좁으면(560 x P 미만) 한 열로, 확대 화면 위 메뉴도 한 열이다.
- Neo둥근모 Pro에는 화살표(→←↑↓)와 ♥가 없다. 화면 글에는 `>`나 Up/Down처럼 글꼴에 있는 글자를 쓴다 (하트는 `DrawHeart`로 그린다).
- 로고: 연한 하늘색 배경, 하얀 행성 고리(O), 30도 기운 진한 하늘색 선, 선 왼쪽은 노이즈(2026-11-24에 세기를 반으로), 오른쪽은 선명. 행성은 넣지 않는다. 확대 화면의 영역 편집 테두리와 안내 문구도 같은 하늘색과 글꼴이다.

## 테스트할 때 주의

- PowerShell 5에서 한글이 든 스크립트는 UTF-8 BOM으로 저장해야 한다.
- 커서 테스트는 실제 마우스를 움직이고, 사용자가 마우스를 움직이면 결과가 흐트러진다.
- 커서 속도 테스트는 Windows 마우스 속도를 잠시 바꾸므로 끝나면 원래 값을 확인한다.
- 슬로모션 테스트는 `tools/test/slowmorun.ps1`로 실행한다. 단축키 시험에서 Alt+Shift+O가 슬로모션 재생 밖에서 눌리면 `build\OpenScaler.ini`의 값이 바뀌므로 끝나면 확인한다.
- `--fg-log`로 잴 때는 OpenScaler가 끝날 때까지 화면을 계속 움직인다. 2026-11-23에 시점 돌리기를 먼저 멈춰, 끝 1~2초의 멈춘 화면이 "멈춤 14~24%, 게임 46~52fps"로 잡혔다 (실제는 0~4%, 59fps).
- PresentMon은 `--timed`가 지나 기록을 마친 뒤에도 프로세스와 ETW 세션이 남는다. 측정이 끝나면 정리한다. 쌓이면 "error code 1450"으로 시작하지 못한다. 관리자 권한으로 `taskkill /f /im PresentMon.exe`, `logman query -ets`에 보이는 PM 세션을 `logman stop 이름 -ets`로 정리한다.
- 캡처는 주사율을 넘지 못한다. 이 PC에서 창이 초당 4,608번 그려도 캡처는 60fps였다.
- 지금 쓰는 PC는 GTX 1060, 60Hz, 125%와 100% 모니터 혼합이다. 프레임 생성은 다른 기기에서 테스트할 예정이라 이 PC의 성능은 제약으로 보지 않는다.
- 실제 게임에서의 체감은 사용자만 확인할 수 있다. 확인하지 못한 것은 그렇게 말한다.
