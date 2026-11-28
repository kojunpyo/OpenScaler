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

// 영역 계산 시간 비교: 슬로모션 덤프의 게임 프레임을 화면 전체와 영역(x, y, 너비, 높이 비율)으로 번갈아 처리하며 GPU 시간을 잰다.
int RunFrameGenRegionBench(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                           const float region[4]);

// 표시 간격 시험: 게임 fps와 주사율을 바꿔 가며, 화면마다 보여 준 게임 속 시간이 고르게 나아가는지 잰다.
// 캡처 시각은 실제처럼 화면 갱신 시각에 맞춰진다 (60Hz에서 45fps면 간격이 16.7, 16.7, 33.3ms로 들쭉날쭉).
// 1.6.0 처리(고정 픽셀, 장면 전환, 밝기 변화)만 껐을 때와 켰을 때 비교한다 (자체 테스트의 마지막 부분).
int RunFrameGenFeatureTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);

int RunFrameGenPaceTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir);

// RIFE 신경망 시간만 입력 크기별로 잰다. 화면과 캡처를 쓰지 않는다.
int RunRifeBench(const std::wstring& outDir);

// 연속 재생 시험 (--fg-seqtest 덤프 출력 [--seq-modes 0,15,63] [--seq-box 이름:x,y,w,h] [--seq-save 쌍,...] [--seq-profile 파일.json]
// [--seq-step 2] [--seq-grid 30]): 녹화의 진짜 프레임을 step장씩 건너뛰어 차례로 넣고 건너뛴 장과 비교한다.
struct SeqTestBox {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;  // 원본 픽셀
};
struct SeqTestOptions {
    std::vector<int> modes = { 0, 15, 63 };  // 비교할 FgFeature 조합
    std::vector<SeqTestBox> boxes;           // 따로 화질을 잴 곳
    std::vector<int> save;                   // 결과를 저장할 쌍 번호
    std::wstring profile;                    // 고정 UI 상자를 읽고 합쳐 저장할 프로필
    int step = 2;                            // 이 장수마다 하나씩 넣는다 (가운데 장이 정답)
    int gridEvery = 30;                      // 이 쌍마다 고정 UI 상자를 다시 묶는다 (실제는 5초마다)
    int loops = 1;                           // 녹화를 이만큼 이어서 반복한다 (3초 녹화로 오래 쌓이는 보호를 본다)
};
int RunFrameGenSeqTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                       const SeqTestOptions& options);

// RGBA 영상을 BMP로 저장한다 (테스트용)
void SaveFrameBmp(const std::wstring& path, const std::vector<uint32_t>& rgba, int w, int h);
