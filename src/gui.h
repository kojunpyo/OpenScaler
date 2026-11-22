#pragma once

// OpenScaler 설정 창 (Dear ImGui)

#include <windows.h>
#include <d3d11.h>

// 캡처/렌더링과 같은 D3D 디바이스로 설정 창을 만든다.
bool GuiCreate(ID3D11Device* device, ID3D11DeviceContext* context);
void GuiDestroy();

// 설정 창을 한 프레임 그린다.
// throttled: 확대 중일 때. 수직동기를 기다리지 않고 0.1초에 한 번만 그린다.
void GuiRender(bool throttled);

bool GuiIsOpen();     // 사용자가 창을 닫으면 false (프로그램 종료)
bool GuiIsVisible();  // 최소화되어 있으면 false
HWND GuiHwnd();
HWND GuiSelectedWindow();  // 설정 창에서 고른 대상 창

// 확대 화면 위 메뉴 (모니터가 하나라 설정 창이 확대 화면에 가려질 때). 확대 화면을 그린 뒤 Present 전에 부른다.
enum OverlayPrompt {
    OVERLAY_PROMPT_NONE,
    OVERLAY_PROMPT_ASK_REGION,   // "프레임 생성 영역을 지정할까요?" 예 / 아니오
    OVERLAY_PROMPT_COMPUTING,    // "추천 영역 계산 중..." (누를 수 없는 안내, 게임은 계속 조작)
    OVERLAY_PROMPT_EVERYWHERE,   // 화면 대부분이 움직여 좁힐 곳이 없음: 직접 지정하기 / 닫기
};
enum OverlayAction { OVERLAY_ACTION_NONE, OVERLAY_ACTION_CLOSE_MENU, OVERLAY_ACTION_YES, OVERLAY_ACTION_NO, OVERLAY_ACTION_MANUAL, OVERLAY_ACTION_DISMISS };
struct OverlayView {
    bool menu = false;               // 설정 패널을 그린다
    int prompt = OVERLAY_PROMPT_NONE;
    double seconds = 0, needSeconds = 10;  // 계산 중: 모은 시간과 필요한 시간
    bool still = false;              // 계산 중: 화면이 거의 움직이지 않는다
};
// 돌려주는 값은 OverlayAction (누른 버튼)
int GuiOverlayRender(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int width, int height,
                     const OverlayView& view);
void GuiOverlayMouse(UINT msg, WPARAM wp, LPARAM lp);  // 확대 화면 창이 받은 마우스 메시지 (창 좌표)
void GuiOverlayReset();     // 메뉴를 닫을 때 눌린 버튼 상태를 지운다
void GuiOverlayShutdown();  // 프로그램 끝에서
