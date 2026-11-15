#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

// 계산으로 만든 영상으로 프레임 생성 품질과 GPU 시간을 재서 outDir에 저장한다.
int RunFrameGenSelfTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);

// 슬로모션 덤프(--slowmo-dump)의 진짜 프레임으로 간격별 생성 품질을 잰다. 가운데 진짜 프레임이 정답이다.
int RunFrameGenGapTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir);

// RGBA 영상을 BMP로 저장한다 (테스트용)
void SaveFrameBmp(const std::wstring& path, const std::vector<uint32_t>& rgba, int w, int h);
