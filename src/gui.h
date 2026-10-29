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
