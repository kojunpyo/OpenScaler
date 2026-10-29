#pragma once

// main.cpp(캡처/렌더링)와 gui.cpp(설정 창)가 함께 쓰는 설정과 기능

#include <windows.h>
#include <string>

enum Mode { MODE_NEAREST, MODE_BILINEAR, MODE_BICUBIC, MODE_LANCZOS, MODE_FSR, MODE_COUNT };
extern const char* const kModeNames[MODE_COUNT];

struct Settings {
    int mode = MODE_FSR;
    float sharpness = 0.5f;
    bool integerScale = false;
    bool compare = false;            // 화면 분할 비교: 왼쪽은 Bilinear(일반 확대), 오른쪽은 현재 모드
    bool osdAlways = false;          // 화면 위 상태 표시를 항상 켜 둔다 (끄면 설정을 바꿀 때만 잠깐 표시)
    bool adjustCursorSpeed = false;  // 확대 중 Windows 마우스 속도를 배율만큼 낮춰 화면상 커서 속도를 원래대로 맞춘다
};

struct ScalingStatus {
    bool active = false;
    HWND target = nullptr;
    unsigned srcW = 0, srcH = 0;  // 원본 (게임 화면)
    unsigned dstW = 0, dstH = 0;  // 확대 결과
    double outputFps = 0;
    double captureFps = 0;
};

// 현재 설정. 값을 바꾼 뒤에는 AppSettingsChanged()를 불러 저장하고 화면에 잠깐 표시한다.
Settings& AppSettings();
void AppSettingsChanged();

void AppStartScaling(HWND target);
void AppStopScaling();
ScalingStatus AppGetStatus();
const std::wstring& AppSettingsPath();
