#pragma once

#include <d3d11.h>
#include <string>

// 계산으로 만든 영상으로 프레임 생성 품질과 GPU 시간을 재서 outDir에 저장한다.
int RunFrameGenSelfTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);
