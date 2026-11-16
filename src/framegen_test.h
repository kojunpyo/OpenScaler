#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

// 계산으로 만든 영상으로 프레임 생성 품질과 GPU 시간을 재서 outDir에 저장한다.
int RunFrameGenSelfTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);

// 슬로모션 덤프(--slowmo-dump)의 진짜 프레임으로 간격별 생성 품질을 잰다. 가운데 진짜 프레임이 정답이다.
// profilePath를 주면 그 게임 프로필의 학습된 설정과 화면 지도를 기본값과 비교한다.
int RunFrameGenGapTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                       const std::wstring& profilePath);

// 프레임 생성 영역 시험: 영역 안은 그 부분만 넣었을 때와 같고, 영역 밖은 게임 프레임 그대로인지 확인한다.
int RunFrameGenRegionTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);

// RIFE 신경망 시간만 입력 크기별로 잰다. 화면과 캡처를 쓰지 않는다.
int RunRifeBench(const std::wstring& outDir);

// RGBA 영상을 BMP로 저장한다 (테스트용)
void SaveFrameBmp(const std::wstring& path, const std::vector<uint32_t>& rgba, int w, int h);
