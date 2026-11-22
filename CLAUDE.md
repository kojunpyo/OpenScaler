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
- `third_party/`: fsr(MIT), imgui(MIT), galmuri(OFL 1.1), onnxruntime 헤더(MIT)
- `tools/make_icon.ps1`: 로고와 아이콘 생성
- `tools/profile_info.py`: 학습 데이터 요약, 비교, 형식 검사 (CI가 `shared-profiles/`를 검사). 공유 방법은 `docs/sharing-profiles.md`
- `tools/test/`: 수동 테스트 스크립트 (`demoform.ps1`: README 시연 화면용 창, `devicecheck.ps1`: 다른 기기에서 기록 모으기, `fgtiming.py`/`fgquality.py`: README 평가 기준으로 등급 매기기)

렌더링 순서: 캡처 -> 보이는 클라이언트 영역 복사 -> 프레임 생성(켠 경우) -> Pass 1 업스케일 -> Pass 2 샤프닝 -> 비교 모드 -> 커서 -> Direct2D 상태 표시 -> Present.

## 현재 상태

- 코드 버전 1.3.8, 공개 릴리스 v1.3.8 (2026-11-22: README 시연 화면을 일반 확대와 OpenScaler를 나란히 붙인 게임 영상으로 교체, 코드 변경 없음). 이전 공개 릴리스 v1.3.7 (2026-11-22: 영역 추천, 확대 화면 위 메뉴 Alt+Shift+G, 확대하면 메뉴와 "프레임 생성 영역을 지정할까요?" 팝업). 이전 공개 릴리스 v1.3.4 (144Hz 끊김 개선: 학습 채점을 나눠 돌림). 고주사율 확인은 2026-11-22에 했다 (주 모니터를 144Hz로, NVIDIA 전원 관리 "최대 성능 선호"가 필요했다). 이전 공개 릴리스 v1.3.2 (2026-11-18, 영역 계산 시간 벤치마크와 README 목차. v1.3.0에서 프레임 생성 영역과 학습 데이터 공유 추가). README 제목에는 번호가 붙어 있으니 앵커 링크는 `#4-게임별-학습`처럼 번호까지 쓴다. 고주사율 확인은 끝났고 남은 개선 후보는 아래 "다음 작업"에 있다.
- 지켜야 할 결정:
  - 프레임 생성 방식은 블록 매칭 보간(기본값) / RIFE 보간 / 블록 매칭 외삽 하나로 고른다 (`FgModeOf`, `SetFgMode`). 게임별 학습이 블록 매칭에만 적용되므로 블록 매칭이 기본값이다.
  - 게임별 학습은 보간용과 외삽용을 따로 둔다 (`GameProfile::interp`, `extrap`, 프로필의 `x_` 값). RIFE는 학습되지 않는다.
  - DJMAX 테스트는 게임에 Enter 외의 입력을 보내지 않는다 (비정상 동작으로 보고 꺼질 수 있다). 사용자가 곡 선택 화면을 준비해 둔다.
  - 학습 데이터 공유 기준은 사용자가 확인했다: 공유 권장은 최근 채점 200회 이상, `shared-profiles` 파일 교체는 같은 방식의 "기준보다 +dB"가 0.2dB 넘게 높을 때 (`docs/sharing-profiles.md`).
  - 성능은 README "성능 평가 기준"으로 매긴다 (`tools/test/fgtiming.py`, `fgquality.py`).
  - 확대를 시작하면 메뉴를 열고 "프레임 생성 영역을 지정할까요?" 팝업을 띄운다 (문구는 사용자가 정함, `kAskRegionTitle`). 설정 창을 쓸 때만이고 `--no-gui` 측정과 시험에서는 띄우지 않는다 (`--region-wizard`로 강제).
- 다음 작업 (자세한 내용과 측정 기록은 `docs/frame-generation-plan.md`):
  1. 고주사율 확인 결과(`docs/frame-generation-plan.md` "고주사율 확인")에서 남은 것: 화면이 큰 게임(DJMAX 1280x720)은 144Hz에서 화면 전체 프레임 생성이 분당 50~198번 끊긴다. 건반 영역을 지정하면 분당 4~19번. 1.3.5에서 영역 추천 버튼을 만들었다 (`FrameGen::RecommendRegion`, 시험 `--region-recommend 초`). 움직임 찾기를 두 화면에 나누는 방법은 효과가 없었다.
  2. 그 결과로 정할 개선 후보: RIFE 지연 줄이기(1.3.3에서 끊김은 없앴지만 추가 지연 44ms), 고정 UI와 조준점 깨짐(화면 지도의 고정 칸 보호), 정수배가 아닌 배수의 화면 간격, 장면 전환 감지, 게임별 학습과 RIFE 연결, 긴 간격에서 밝기가 바뀌는 글자 깨짐.
  3. README 마무리: 평가 표의 고주사율 결과는 2026-11-23에 채웠다 (요약, RIFE 영역 없음, 2배 고정). 시연 화면은 2026-11-22에 유튜브 게임 영상으로 바꿨다 (`docs/demo.webp`, GIF로는 22MB가 넘어 같은 크기와 장 수의 WebP로 넣음).
  4. 운영: 이슈/PR 템플릿, CONTRIBUTING.md.

## 사용 중인 단축키

Alt+Shift + S(시작/중지), M(모드), I(정수배), Up/Down(샤프닝), C(비교), O(상태 표시), P(커서 속도), F(프레임 생성), T(슬로모션 테스트), G(확대 화면 위 메뉴), Q(종료).
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
