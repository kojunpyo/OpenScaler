#pragma once

// 런타임에 D3DCompile로 컴파일되는 HLSL 소스.
// Pass 1 (PSScale)  : 캡처한 원본을 출력 크기로 업스케일
// Pass 2 (PSSharpen): 업스케일 결과에 대비 적응형 샤프닝(CAS 아이디어 기반) 적용
static const char kShaderSource[] = R"(
cbuffer Params : register(b0)
{
    float2 srcSize;
    float2 invSrcSize;
    int    mode;
    float  sharpness;
    float2 pad;
};

Texture2D<float4> tex     : register(t0);
SamplerState      linSamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// 정점 버퍼 없이 화면 전체를 덮는 삼각형 하나를 그린다.
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 Fetch(int2 p)
{
    p = clamp(p, int2(0, 0), int2(srcSize) - 1);
    return tex.Load(int3(p, 0)).rgb;
}

float3 Nearest(float2 uv)
{
    return Fetch(int2(floor(uv * srcSize)));
}

float3 Bilinear(float2 uv)
{
    return tex.SampleLevel(linSamp, uv, 0).rgb;
}

// Catmull-Rom 바이큐빅. 16탭 대신 선형 샘플러를 이용해 9탭으로 계산한다.
float3 Bicubic(float2 uv)
{
    float2 samplePos = uv * srcSize;
    float2 tc1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - tc1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 tc0  = (tc1 - 1) * invSrcSize;
    float2 tc3  = (tc1 + 2) * invSrcSize;
    float2 tc12 = (tc1 + w2 / w12) * invSrcSize;

    float3 r = 0;
    r += tex.SampleLevel(linSamp, float2(tc0.x,  tc0.y),  0).rgb * w0.x  * w0.y;
    r += tex.SampleLevel(linSamp, float2(tc12.x, tc0.y),  0).rgb * w12.x * w0.y;
    r += tex.SampleLevel(linSamp, float2(tc3.x,  tc0.y),  0).rgb * w3.x  * w0.y;
    r += tex.SampleLevel(linSamp, float2(tc0.x,  tc12.y), 0).rgb * w0.x  * w12.y;
    r += tex.SampleLevel(linSamp, float2(tc12.x, tc12.y), 0).rgb * w12.x * w12.y;
    r += tex.SampleLevel(linSamp, float2(tc3.x,  tc12.y), 0).rgb * w3.x  * w12.y;
    r += tex.SampleLevel(linSamp, float2(tc0.x,  tc3.y),  0).rgb * w0.x  * w3.y;
    r += tex.SampleLevel(linSamp, float2(tc12.x, tc3.y),  0).rgb * w12.x * w3.y;
    r += tex.SampleLevel(linSamp, float2(tc3.x,  tc3.y),  0).rgb * w3.x  * w3.y;
    return saturate(r);
}

float Lanczos2Weight(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

// Lanczos2 (4x4 탭) + 링잉 억제: 가장 가까운 2x2 픽셀 범위로 결과를 제한한다.
float3 Lanczos(float2 uv)
{
    float2 p = uv * srcSize - 0.5;
    int2 base = int2(floor(p));
    float2 f = p - base;

    float3 sum = 0;
    float wsum = 0;
    float3 mn = 1e9, mx = -1e9;

    [unroll] for (int y = -1; y <= 2; y++)
    {
        float wy = Lanczos2Weight(y - f.y);
        [unroll] for (int x = -1; x <= 2; x++)
        {
            float w = Lanczos2Weight(x - f.x) * wy;
            float3 c = Fetch(base + int2(x, y));
            sum += c * w;
            wsum += w;
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
            {
                mn = min(mn, c);
                mx = max(mx, c);
            }
        }
    }
    return clamp(sum / wsum, mn, mx);
}

float4 PSScale(VSOut i) : SV_Target
{
    float3 c;
    if (mode == 0)      c = Nearest(i.uv);
    else if (mode == 1) c = Bilinear(i.uv);
    else if (mode == 2) c = Bicubic(i.uv);
    else                c = Lanczos(i.uv);
    return float4(c, 1);
}

// 대비가 낮은 곳은 강하게, 이미 대비가 큰 곳은 약하게 샤프닝한다.
float4 PSSharpen(VSOut i) : SV_Target
{
    int2 p = int2(i.uv * srcSize);
    float3 e = Fetch(p);
    if (sharpness <= 0.0)
        return float4(e, 1);

    float3 b = Fetch(p + int2(0, -1));
    float3 d = Fetch(p + int2(-1, 0));
    float3 f = Fetch(p + int2(1, 0));
    float3 h = Fetch(p + int2(0, 1));

    float3 mn = min(e, min(min(b, d), min(f, h)));
    float3 mx = max(e, max(max(b, d), max(f, h)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
    float3 w = amp * peak;

    float3 c = (b + d + f + h) * w + e;
    return float4(saturate(c / (1.0 + 4.0 * w)), 1);
}

// 커서 이미지를 1:1로 그린다. 블렌딩은 C++ 쪽 블렌드 스테이트가 담당한다.
float4 PSCursor(VSOut i) : SV_Target
{
    int2 p = clamp(int2(i.uv * srcSize), int2(0, 0), int2(srcSize) - 1);
    return tex.Load(int3(p, 0));
}
)";
