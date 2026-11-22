#pragma once

// main.cpp(캡처/렌더링)와 gui.cpp(설정 창)가 함께 쓰는 설정과 기능

#include <windows.h>
#include <string>

struct GameProfile;

enum Mode { MODE_NEAREST, MODE_BILINEAR, MODE_BICUBIC, MODE_LANCZOS, MODE_FSR, MODE_COUNT };
extern const char* const kModeNames[MODE_COUNT];

// 화면 지도 사용. 예전 설정 파일의 2(리듬게임)는 자동과 같게 읽는다.
enum FgPreset { FG_PRESET_AUTO, FG_PRESET_NO_MAP, FG_PRESET_COUNT };

enum SlowMoState { SLOWMO_OFF, SLOWMO_WAITING, SLOWMO_RECORDING, SLOWMO_GENERATING, SLOWMO_PLAYING };

struct Settings {
    int mode = MODE_FSR;
    float sharpness = 0.5f;
    bool integerScale = false;
    bool compare = false;            // 화면 분할 비교: 왼쪽은 Bilinear(일반 확대), 오른쪽은 현재 모드
    bool osdAlways = false;          // 화면 위 상태 표시를 항상 켜 둔다 (끄면 설정을 바꿀 때만 잠깐 표시)
    bool adjustCursorSpeed = false;  // 확대 중 Windows 마우스 속도를 배율만큼 낮춰 화면상 커서 속도를 원래대로 맞춘다

    bool frameGen = false;           // 프레임 생성 (보간 또는 외삽)
    int fgMultiplier = 0;            // 0: 주사율까지 자동으로 채우기, 2~4: 진짜 프레임 사이를 그 수만큼 나눈다
    int fgQuality = 1;               // 0: 빠름, 1: 기본, 2: 고품질
    int fgMethod = 0;                // 0: 보간, 1: 외삽(실험)
    int fgGenerator = 0;             // 0: 블록 매칭 (기본값, 게임별 학습이 맞춘다), 1: RIFE
    int fgPreset = FG_PRESET_AUTO;   // 화면 지도: 자동이면 채점에서 더 좋을 때만 쓴다
    bool fgLearn = true;             // 게임별 학습
    bool fgVisualize = false;        // 움직임을 색으로 표시 (발표용, 저장하지 않는다)

    int slowMoTargetFps = 120;       // 슬로모션 테스트에서 가정하는 출력 fps
    int slowMoSeconds = 1;           // 슬로모션 테스트의 녹화 길이 (초)
};

// 설정 창의 "방식": 생성기와 보간/외삽을 하나로 묶어 고르게 한다. 외삽은 블록 매칭으로만 만든다.
enum FgMode { FG_MODE_BLOCK, FG_MODE_RIFE, FG_MODE_EXTRAPOLATE, FG_MODE_COUNT };
inline int FgModeOf(const Settings& s)
{
    if (s.fgMethod == 1) return FG_MODE_EXTRAPOLATE;
    return s.fgGenerator == 1 ? FG_MODE_RIFE : FG_MODE_BLOCK;
}
inline void SetFgMode(Settings& s, int mode)
{
    s.fgMethod = mode == FG_MODE_EXTRAPOLATE ? 1 : 0;
    s.fgGenerator = mode == FG_MODE_RIFE ? 1 : 0;
}

struct ScalingStatus {
    bool active = false;
    HWND target = nullptr;
    unsigned srcW = 0, srcH = 0;  // 원본 (게임 화면)
    unsigned dstW = 0, dstH = 0;  // 확대 결과
    double outputFps = 0;
    double captureFps = 0;

    // 프레임 생성
    bool fgActive = false;
    bool fgNoRoom = false;       // 진짜 프레임이 주사율만큼 나와 넣을 자리가 없다
    double fgRealFps = 0;
    double fgEstimateMs = 0;     // 움직임 추정 (진짜 프레임마다)
    double fgSynthMs = 0;        // 합성 (화면을 그릴 때마다)
    int fgRifeState = -1;        // -1: 쓰지 않음, 0: 준비 중, 1: 사용 중, 2: 실패
    double fgRifeMs = 0;         // 신경망 한 번에 걸린 시간

    // 슬로모션 테스트
    int slowMoState = SLOWMO_OFF;
    int slowMoDone = 0;          // 만든 프레임 수, 재생 중이면 지금 몇 번째인지
    int slowMoTotal = 0;
    double refreshHz = 60;
};

// 현재 설정. 값을 바꾼 뒤에는 AppSettingsChanged()를 불러 저장하고 화면에 잠깐 표시한다.
Settings& AppSettings();
void AppSettingsChanged();

void AppStartScaling(HWND target);
void AppStopScaling();
void AppSlowMoToggle(double delaySeconds);  // 슬로모션 테스트 시작(이 시간 뒤 녹화) 또는 끝내기
// 프레임 생성 영역 (확대 중인 게임의 프로필에 저장한다)
void AppRegionEditBegin();          // 확대 화면 위에 영역 편집 창을 띄운다
void AppRegionEditEnd(bool save);   // 편집 창을 닫는다. save면 영역을 저장한다
bool AppRegionEditing();
void AppRegionClear();              // 영역을 지워 화면 전체에 만든다
// 영역 추천: 최근 게임 화면에서 자주 바뀐 곳으로 영역 편집 창을 띄운다.
// 0: 띄움, 1: 모은 시간이 모자람, 2: 화면이 거의 안 움직임, 3: 화면 대부분이 움직임, -1: 프레임 생성이 꺼져 있음
int AppRegionRecommend();
// 학습 데이터 공유 (docs/sharing-profiles.md)
bool AppImportProfile(const std::wstring& file, std::wstring& gameName, std::wstring& message);
bool AppExportProfile(const std::wstring& gameName, const std::wstring& file);
ScalingStatus AppGetStatus();
const GameProfile* AppActiveProfile();  // 지금 확대 중인 게임의 프로필. 없으면 nullptr
const std::wstring& AppSettingsPath();
