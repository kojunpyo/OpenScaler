// AMD FidelityFX Super Resolution 1.0 (FSR 1.0)
//   PSEasu: Edge Adaptive Spatial Upsampling - 윤곽선 방향을 보고 보간하는 업스케일러
//   PSRcas: Robust Contrast Adaptive Sharpening - FSR 전용 샤프닝
// 알고리즘 본체는 third_party/fsr의 AMD 공식 헤더(MIT 라이선스)를 그대로 사용하고,
// 여기서는 입력 텍스처를 읽는 콜백과 픽셀 셰이더 진입점만 정의한다.
// 빌드할 때 fxc로 미리 컴파일된다 (CMakeLists.txt 참고).

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer FsrParams : register(b1)
{
    uint4 easuCon0;
    uint4 easuCon1;
    uint4 easuCon2;
    uint4 easuCon3;
    uint4 rcasCon;
    int2  outputOffset;  // RCAS 출력이 백버퍼 안에서 시작하는 위치
    int2  pad;
};

Texture2D<float4> tex     : register(t0);
SamplerState      linSamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return tex.GatherRed(linSamp, p); }
AF4 FsrEasuGF(AF2 p) { return tex.GatherGreen(linSamp, p); }
AF4 FsrEasuBF(AF2 p) { return tex.GatherBlue(linSamp, p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return tex.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

float4 PSEasu(VSOut i) : SV_Target
{
    AF3 c;
    FsrEasuF(c, AU2(i.pos.xy), easuCon0, easuCon1, easuCon2, easuCon3);
    return float4(c, 1);
}

float4 PSRcas(VSOut i) : SV_Target
{
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(int2(i.pos.xy) - outputOffset), rcasCon);
    return float4(c, 1);
}
