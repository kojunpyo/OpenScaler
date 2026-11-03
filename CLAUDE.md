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
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build 2>&1"
```

- 결과물은 `buildOpenScaler.exe`. 빌드 후 프로젝트 루트의 `OpenScaler.exe`로 복사해 사용자가 쓰게 한다.
- 사용자가 루트 exe를 실행 중이면 복사가 실패한다. 프로세스를 끄지 말고 알린다.
- exe, `build/`, `OpenScaler.ini`, `profiles/`, `rife/`는 git에 올리지 않는다.
- RIFE를 시험하려면 `rife` 폴더를 `build\`에도 두어야 한다 (`build\OpenScaler.exe`는 자기 옆에서 찾는다).
- GitHub CLI(`gh`)는 없다. Actions 결과는 공개 API로 확인한다 (`tools/test/waitci.ps1`).

## 구조

- `src/main.cpp`: 캡처(Windows.Graphics.Capture), 오버레이 창, D3D11 렌더링, 커서 좌표 변환과 속도 보정, 설정 파일, 단축키, 메인 루프
- `src/app.h`: main.cpp와 gui.cpp가 함께 쓰는 설정과 기능
- `src/gui.cpp`: 설정 창 (Dear ImGui 1.91.5)
- `src/shaders.h`: 실행 중에 컴파일하는 HLSL (업스케일, 샤프닝, 커서)
- `src/framegen.*`, `src/framegen_shaders.h`: 프레임 생성 (움직임 추정, 합성, 표시 시점, 채점)
- `src/framegen_test.*`: 프레임 생성 자체 테스트 (`--fg-selftest 폴더`, 화면과 마우스를 쓰지 않음)
- `src/game_profile.*`: 게임별 프로필 (설정 자동 조정, 화면 지도, `profiles/*.json`)
- `src/rife.*`: RIFE 실행 (실행 파일 옆 `rife` 폴더의 ONNX Runtime과 모델을 실행 중에 불러옴). `tools/rife_export.py`: 모델 변환
- `src/fsr.hlsl`, `src/fsr_constants.*`: FSR 1.0. fxc로 빌드 때 컴파일
- `third_party/`: fsr(MIT), imgui(MIT), galmuri(OFL 1.1), onnxruntime 헤더(MIT)
- `tools/make_icon.ps1`: 로고와 아이콘 생성
- `tools/test/`: 수동 테스트 스크립트

렌더링 순서: 캡처 -> 보이는 클라이언트 영역 복사 -> 프레임 생성(켠 경우) -> Pass 1 업스케일 -> Pass 2 샤프닝 -> 비교 모드 -> 커서 -> Direct2D 상태 표시 -> Present.

## 현재 상태

- 코드 버전 1.0.0 (`main`). 공개 릴리스는 v0.3.0까지이며 v1.0.0은 아직 내지 않았다.
- 완료: 창 캡처, 5개 업스케일 모드(기본 FSR 1.0), 샤프닝, 정수배, 비교 모드, 상태 표시, 설정 저장, 커서 좌표 변환, 커서 속도 보정, 지연 감소, 설정 창, 로고와 아이콘, CI와 자동 릴리스
- 프레임 생성: 블록 매칭, 학습 1, 2단계, RIFE 생성기 구현 완료 (옵션 B, 커밋 전). 실제 게임에서는 아직 확인하지 못했다.
- 다음 작업: 실제 게임과 테스트 기기에서 확인. 결정과 측정 결과는 `docs/frame-generation-plan.md`
- 남은 운영 작업: 이슈/PR 템플릿, CONTRIBUTING.md

## 사용 중인 단축키

Alt+Shift + S(시작/중지), M(모드), I(정수배), Up/Down(샤프닝), C(비교), O(상태 표시), P(커서 속도), F(프레임 보간), T(슬로모션 테스트), Q(종료).
슬로모션 재생 중에만 Space(멈춤), Left/Right(한 장씩)를 등록한다. 이 PC에서 Alt+Shift+R은 다른 프로그램이 이미 쓰고 있어 등록되지 않는다.

## 설정 창 디자인 (사용자 지정)

- 흰 바탕, 각진 모서리, 두꺼운 픽셀 테두리, 언더테일 느낌. 구역 제목은 `* `로 시작하고 픽셀 하트를 쓴다.
- 대상 창 구역은 분홍. 업스케일, 화면 표시 · 마우스, 단축키는 진하기가 다른 회색 (`kPinkPalette`, `kGrayPalette`).
- 제목은 Open 연분홍, Scaler 진한 분홍.
- 옵션 이름은 Galmuri11 Bold, 설명은 얇은 Galmuri11 Condensed. 글꼴은 12px의 정수 배로만 그린다.
- Galmuri 일반체는 쓰지 않는다. 원작자 설명상 다른 글꼴에서 온 글리프가 섞여 있어, 전부 창작물인 Bold와 Condensed만 쓴다.
- 설명 글꼴은 `kSecondaryTexts`와 `kModeDescriptions`에 있는 글자만 담는다. 새 설명 문구는 여기에 넣어야 글자가 깨지지 않는다.
- 로고: 연분홍 배경, 하얀 행성 고리(O), 30도 기운 진한 분홍 선, 선 왼쪽은 노이즈, 오른쪽은 선명. 행성은 넣지 않는다.

## 테스트할 때 주의

- PowerShell 5에서 한글이 든 스크립트는 UTF-8 BOM으로 저장해야 한다.
- 커서 테스트는 실제 마우스를 움직이고, 사용자가 마우스를 움직이면 결과가 흐트러진다.
- 커서 속도 테스트는 Windows 마우스 속도를 잠시 바꾸므로 끝나면 원래 값을 확인한다.
- 슬로모션 테스트는 `tools/test/slowmorun.ps1`로 실행한다. 단축키 시험에서 Alt+Shift+O가 슬로모션 재생 밖에서 눌리면 `build\OpenScaler.ini`의 값이 바뀌므로 끝나면 확인한다.
- 캡처는 주사율을 넘지 못한다. 이 PC에서 창이 초당 4,608번 그려도 캡처는 60fps였다.
- 지금 쓰는 PC는 GTX 1060, 60Hz, 125%와 100% 모니터 혼합이다. 프레임 생성은 다른 기기에서 테스트할 예정이라 이 PC의 성능은 제약으로 보지 않는다.
- 실제 게임에서의 체감은 사용자만 확인할 수 있다. 확인하지 못한 것은 그렇게 말한다.
