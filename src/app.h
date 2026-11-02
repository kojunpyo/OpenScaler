#pragma once

// main.cpp(캡처/렌더링)와 gui.cpp(설정 창)가 함께 쓰는 설정과 기능

#include <windows.h>
#include <string>

struct GameProfile;

enum Mode { MODE_NEAREST, MODE_BILINEAR, MODE_BICUBIC, MODE_LANCZOS, MODE_FSR, MODE_COUNT };
extern const char* const kModeNames[MODE_COUNT];

enum FgPreset { FG_PRESET_AUTO, FG_PRESET_GENERAL, FG_PRESET_RHYTHM, FG_PRESET_COUNT };

struct Settings {
    int mode = MODE_FSR;
    float sharpness = 0.5f;
    bool integerScale = false;
    bool compare = false;            // 화면 분할 비교: 왼쪽은 Bilinear(일반 확대), 오른쪽은 현재 모드
    bool osdAlways = false;          // 화면 위 상태 표시를 항상 켜 둔다 (끄면 설정을 바꿀 때만 잠깐 표시)
    bool adjustCursorSpeed = false;  // 확대 중 Windows 마우스 속도를 배율만큼 낮춰 화면상 커서 속도를 원래대로 맞춘다

    bool frameGen = false;           // 프레임 보간
    int fgMultiplier = 0;            // 0: 주사율까지 자동으로 채우기, 2~4: 진짜 프레임 사이를 그 수만큼 나눈다
    int fgQuality = 1;               // 0: 빠름, 1: 기본, 2: 고품질
    int fgMethod = 0;                // 0: 보간, 1: 외삽(실험)
    int fgGenerator = 0;             // 0: 블록 매칭, 1: RIFE(실험)
    int fgPreset = FG_PRESET_AUTO;   // 게임 유형. 자동이면 화면 통계로 판단한다
    bool fgLearn = true;             // 게임별 학습
    bool fgVisualize = false;        // 움직임을 색으로 표시 (발표용, 저장하지 않는다)
};

struct ScalingStatus {
    bool active = false;
    HWND target = nullptr;
    unsigned srcW = 0, srcH = 0;  // 원본 (게임 화면)
    unsigned dstW = 0, dstH = 0;  // 확대 결과
    double outputFps = 0;
    double captureFps = 0;

    // 프레임 보간
    bool fgActive = false;
    bool fgNoRoom = false;       // 진짜 프레임이 주사율만큼 나와 넣을 자리가 없다
    double fgRealFps = 0;
    double fgEstimateMs = 0;     // 움직임 추정 (진짜 프레임마다)
    double fgSynthMs = 0;        // 합성 (화면을 그릴 때마다)
    int fgRifeState = -1;        // -1: 쓰지 않음, 0: 준비 중, 1: 사용 중, 2: 실패
    double fgRifeMs = 0;         // 신경망 한 번에 걸린 시간
};

// 현재 설정. 값을 바꾼 뒤에는 AppSettingsChanged()를 불러 저장하고 화면에 잠깐 표시한다.
Settings& AppSettings();
void AppSettingsChanged();

void AppStartScaling(HWND target);
void AppStopScaling();
ScalingStatus AppGetStatus();
const GameProfile* AppActiveProfile();  // 지금 확대 중인 게임의 프로필. 없으면 nullptr
const std::wstring& AppSettingsPath();
