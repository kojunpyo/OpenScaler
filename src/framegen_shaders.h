#pragma once

// 프레임 생성용 컴퓨트 셰이더 (실행 중에 D3DCompile로 컴파일한다)
//
// 움직임 추정: CSLuma, CSDown -> CSMatch(굵은 단계부터 세밀한 단계로) -> CSConsist
// 합성:        CSProject -> CSFill -> CSCompose
// 평가/표시:   CSBlend, CSMse, CSVisualize
//
// MSVC 문자열 길이 제한(16KB) 때문에 몇 조각으로 나눠 이어 붙인다.
static const char kFrameGenShaderSource[] = R"(
cbuffer FgCB : register(b0)
{
    int2  srcSize;       // 원본 크기
    int2  gridSize;      // 이번 단계의 움직임 격자 크기
    int2  lvlSize;       // 이번 단계의 휘도 영상 크기
    int2  prevGridSize;  // 한 단계 굵은 격자 크기
    int   cellLvl;       // 격자 한 칸의 크기 (이번 단계 픽셀)
    int   levelScale;    // 이번 단계 1픽셀이 원본 몇 픽셀인지
    int   prevCellSrc;   // 한 단계 굵은 격자 한 칸의 크기 (원본 픽셀)
    int   cellSrc;       // 이번 격자 한 칸의 크기 (원본 픽셀)
    int   searchRadius;  // 가장 굵은 단계의 전체 탐색 범위 (단계 픽셀)
    int   coarsest;      // 1이면 전체 탐색
    int   finalPass;     // 1이면 소수점 이하까지 구한다
    int   hasHint;       // 1이면 게임별 화면 지도의 움직임을 후보에 넣는다
    float smoothness;    // 주변과 다른 벡터에 주는 벌점 (원본 픽셀당)
    float t;             // A(0)와 B(1) 사이의 시점. 1보다 크면 외삽
    float blendTol;      // 합성할 때 허용하는 밝기 차이
    float hintScale;     // 화면 지도(픽셀/초)에 곱할 값: A->B는 +프레임 간격, B->A는 -프레임 간격
    int2  temporalGrid;  // 이전 프레임 움직임 격자 크기
    int   temporalCell;  // 이전 프레임 움직임 격자 한 칸의 크기 (원본 픽셀)
    float zeroBias;      // 움직임 없음을 조금 우대한다
    int   wideRadius;    // 0보다 크면 오차가 큰 칸만 이 범위까지 전체 탐색한다 (작고 빠른 물체용)
    int   features;      // FgFeature 조합 (1: 고정 픽셀, 4: 밝기 변화)
    int2  padding;
};

SamplerState linSamp : register(s0);

// ---------------------------------------------------------------------------
// 휘도 피라미드
// ---------------------------------------------------------------------------

Texture2D<float4> lumaColor : register(t0);
Texture2D<float>  lumaFine  : register(t1);
RWTexture2D<float> lumaOut  : register(u0);

[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)lvlSize)) return;
    lumaOut[id.xy] = dot(lumaColor.Load(int3(id.xy, 0)).rgb, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)lvlSize)) return;
    int2 p = int2(id.xy) * 2;
    lumaOut[id.xy] = 0.25 * (lumaFine.Load(int3(p, 0)) + lumaFine.Load(int3(p + int2(1, 0), 0)) +
                             lumaFine.Load(int3(p + int2(0, 1), 0)) + lumaFine.Load(int3(p + int2(1, 1), 0)));
}

// ---------------------------------------------------------------------------
// 블록 매칭: 기준 영상의 블록이 대상 영상의 어디로 갔는지 찾는다.
// 굵은 단계의 결과, 이전 프레임의 결과, 게임별 화면 지도의 움직임을 후보로 쓴 뒤 주변을 더 찾는다.
// 결과: xy = 움직임(원본 픽셀), z = 블록 오차
// ---------------------------------------------------------------------------

Texture2D<float>  matchRef      : register(t0);
Texture2D<float>  matchTgt      : register(t1);
Texture2D<float4> matchPrev     : register(t2);
Texture2D<float4> matchTemporal : register(t3);
Texture2D<float4> matchHint     : register(t4);
RWTexture2D<float4> flowOut     : register(u0);

// 블록 오차. 칸보다 넓은 범위(앞뒤로 2칸)를 격자 무늬로 13점만 비교하고, 화면 밖으로 나간 점은 뺀다.
// 범위가 넓어야 노트 안쪽처럼 무늬 없는 칸도 물체의 가장자리를 보고 위치를 찾을 수 있다.
static const int2 kBlockTaps[13] = {
    int2(-2, -2), int2(0, -2), int2(2, -2), int2(-1, -1), int2(1, -1), int2(-2, 0), int2(0, 0),
    int2(2, 0), int2(-1, 1), int2(1, 1), int2(-2, 2), int2(0, 2), int2(2, 2)
};

float BlockCost(int2 c, int2 v)
{
    int s = max(cellLvl, 1);
    float sum = 0;
    float count = 0;
    // 분기 없이 계산한다. 이 함수가 움직임 추정 시간의 대부분을 차지한다.
    [unroll] for (int i = 0; i < 13; i++)
    {
        int2 o = c + kBlockTaps[i] * s;
        int2 q = o + v;
        int2 clamped = clamp(q, int2(0, 0), lvlSize - 1);
        float inside = (q.x == clamped.x && q.y == clamped.y) ? 1.0 : 0.0;
        float a = matchRef.Load(int3(clamp(o, int2(0, 0), lvlSize - 1), 0));
        sum += inside * abs(a - matchTgt.Load(int3(clamped, 0)));
        count += inside;
    }
    return count < 4 ? 0.05 : sum / count;  // 대부분 화면 밖이면 알 수 없으므로 주변 벡터와 경쟁할 수 있는 값을 준다
}

float TotalCost(int2 c, int2 v, float2 pred)
{
    float cost = BlockCost(c, v) + smoothness * length(float2(v) - pred) * levelScale;
    if (v.x == 0 && v.y == 0) cost -= zeroBias;
    return cost;
}

#define TRY_CANDIDATE(cand)                         \
    {                                               \
        int2 v_ = (cand);                           \
        float cost_ = TotalCost(c, v_, pred);       \
        if (cost_ < bestCost)                       \
        {                                           \
            bestCost = cost_;                       \
            best = v_;                              \
        }                                           \
    }

[numthreads(4, 4, 1)]
void CSMatch(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    int2 c = int2(id.xy) * cellLvl + cellLvl / 2;
    int2 best = int2(0, 0);
    float bestCost = 1e9;
    float2 pred = float2(0, 0);

    // 두 프레임이 그 자리에서 같으면 움직임이 없는 것이다. 무늬 없는 배경에 엉뚱한 벡터가 생기는 것을 막고 계산도 줄인다.
    if (BlockCost(c, int2(0, 0)) < 0.004)
    {
        flowOut[id.xy] = float4(0, 0, 0, 1);
        return;
    }

    if (coarsest)
    {
        [loop] for (int y = -searchRadius; y <= searchRadius; y++)
            [loop] for (int x = -searchRadius; x <= searchRadius; x++)
                TRY_CANDIDATE(int2(x, y));
    }
    else
    {
        int2 srcC = c * levelScale;
        int2 pc = clamp(srcC / prevCellSrc, int2(0, 0), prevGridSize - 1);
        pred = round(matchPrev.Load(int3(pc, 0)).xy / levelScale);

        // 굵은 단계의 주변 9칸은 같은 벡터인 경우가 많다. 이미 본 벡터는 오차도 같아 결과를 바꾸지 못하므로 건너뛴다.
        int2 tried[11];
        [unroll] for (int k = 0; k < 11; k++)
        {
            int2 v;
            if (k < 9)
            {
                int2 q = clamp(pc + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), prevGridSize - 1);
                v = int2(round(matchPrev.Load(int3(q, 0)).xy / levelScale));
            }
            else if (k == 9)
            {
                v = int2(0, 0);
            }
            else
            {
                if (temporalCell <= 0) break;
                int2 tc = clamp(srcC / temporalCell, int2(0, 0), temporalGrid - 1);
                v = int2(round(matchTemporal.Load(int3(tc, 0)).xy / levelScale));
            }
            bool seen = false;
            [unroll] for (int j = 0; j < k; j++) seen = seen || all(tried[j] == v);
            tried[k] = v;
            [branch] if (!seen) TRY_CANDIDATE(v);
        }
        if (hasHint)
        {
            int2 tile = clamp(srcC * int2(32, 18) / srcSize, int2(0, 0), int2(31, 17));
            float2 h = matchHint.Load(int3(tile, 0)).xy;
            int2 hv = int2(round(hintScale * h / levelScale));
            // 이 게임에서 이 자리에 늘 나타나던 움직임은 비슷한 다른 물체보다 우대한다 (같은 노트가 여러 개일 때).
            // 지도의 벡터는 대략적인 값이므로 실제 오차로 바꿔 두어 아래의 주변 탐색이 더 정확한 값을 찾게 한다.
            if (hv.x != 0 || hv.y != 0)
            {
                float hintCost = TotalCost(c, hv, pred);
                if (hintCost - 0.02 < bestCost)
                {
                    bestCost = hintCost;
                    best = hv;
                }
            }
        }

        [loop] for (int stride = 2; stride >= 1; stride--)
        {
            int2 start = best;
            [loop] for (int y = -1; y <= 1; y++)
                [loop] for (int x = -1; x <= 1; x++)
                    if (x != 0 || y != 0) TRY_CANDIDATE(start + int2(x, y) * stride);
        }

        // 후보들이 모두 맞지 않는 칸: 굵은 단계에서 놓친 작고 빠른 물체일 수 있으므로 넓게 찾는다.
        if (wideRadius > 0 && BlockCost(c, best) > max(0.06, 0.5 * BlockCost(c, int2(0, 0))))
        {
            [loop] for (int y = -wideRadius; y <= wideRadius; y++)
                [loop] for (int x = -wideRadius; x <= wideRadius; x++)
                    TRY_CANDIDATE(int2(x, y));
        }
    }

    float2 sub = float2(0, 0);
    if (finalPass)
    {
        // 오차 곡선을 포물선으로 근사해 소수점 이하 위치를 구한다.
        float c0 = BlockCost(c, best);
        float l = BlockCost(c, best + int2(-1, 0));
        float r = BlockCost(c, best + int2(1, 0));
        float u = BlockCost(c, best + int2(0, -1));
        float d = BlockCost(c, best + int2(0, 1));
        float dx = l + r - 2 * c0;
        float dy = u + d - 2 * c0;
        if (dx > 1e-4) sub.x = clamp((l - r) / (2 * dx), -0.5, 0.5);
        if (dy > 1e-4) sub.y = clamp((u - d) / (2 * dy), -0.5, 0.5);
    }

    flowOut[id.xy] = float4((float2(best) + sub) * levelScale, BlockCost(c, best), 1);
}

// ---------------------------------------------------------------------------
// 양방향 일치 검사: A->B와 B->A가 서로 반대이면 믿을 수 있다.
// 결과: w = 신뢰도 (가려지는 곳, 오차가 큰 곳은 낮다)
// ---------------------------------------------------------------------------

Texture2D<float4> consistOwn   : register(t0);
Texture2D<float4> consistOther : register(t1);

[numthreads(8, 8, 1)]
void CSConsist(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    float4 own = consistOwn.Load(int3(id.xy, 0));
    float2 center = (float2(id.xy) + 0.5) * cellSrc;
    int2 q = clamp(int2((center + own.xy) / cellSrc), int2(0, 0), gridSize - 1);
    float2 other = consistOther.Load(int3(q, 0)).xy;
    float tol = max(2.0, 0.2 * length(own.xy));
    float2 dest = center + own.xy;
    bool outside = any(dest < float2(0, 0)) || any(dest >= float2(srcSize));
    // 화면 밖으로 나간 블록은 반대 방향을 확인할 수 없으므로 중간 신뢰도를 준다.
    float agree = outside ? 0.5 : saturate(1.0 - length(own.xy + other) / (2.0 * tol));
    float conf = agree * saturate(1.0 - own.z * 6.0);
    flowOut[id.xy] = float4(own.xyz, conf);
}
)"
R"(
// ---------------------------------------------------------------------------
// 투영: 각 블록을 시점 t에 있을 자리로 옮겨 적는다.
// 작은 물체(리듬게임의 노트 등)가 배경의 움직임에 묻혀 사라지지 않게 하는 핵심 단계다.
// 한 칸에 여러 블록이 오면 믿을 만하고 많이 움직이는 쪽(대개 앞쪽 물체)을 남긴다.
// ---------------------------------------------------------------------------

Texture2D<float4> projectF01 : register(t0);
Texture2D<float4> projectF10 : register(t1);
RWTexture2D<uint> splatOut   : register(u0);

void Splat(float2 center, float2 displacement, float2 vecAB, float conf)
{
    int2 dest = int2(floor((center + displacement) / cellSrc));
    if (any(dest < int2(0, 0)) || any(dest >= gridSize)) return;
    uint prio = (uint(saturate(conf) * 15.0 + 0.5) << 4) | uint(saturate(length(vecAB) / 64.0) * 15.0 + 0.5);
    uint x = uint(clamp(round(vecAB.x * 4.0) + 2048.0, 0.0, 4095.0));
    uint y = uint(clamp(round(vecAB.y * 4.0) + 2048.0, 0.0, 4095.0));
    uint code = (prio << 24) | (x << 12) | y;
    uint previous;
    InterlockedMax(splatOut[dest], code, previous);
}

[numthreads(8, 8, 1)]
void CSProject(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    float2 center = (float2(id.xy) + 0.5) * cellSrc;
    float4 f01 = projectF01.Load(int3(id.xy, 0));
    float4 f10 = projectF10.Load(int3(id.xy, 0));
    Splat(center, t * f01.xy, f01.xy, f01.w);
    Splat(center, (1.0 - t) * f10.xy, -f10.xy, f10.w);
}

// ---------------------------------------------------------------------------
// 빈칸 채우기: 물체가 비켜나 드러난 곳은 대개 배경이므로 가장 가까운 칸 중 움직임이 작은 벡터를 쓴다.
// ---------------------------------------------------------------------------

Texture2D<uint> fillSplat : register(t0);

float2 UnpackVector(uint v)
{
    return float2(float((v >> 12) & 0xFFF) - 2048.0, float(v & 0xFFF) - 2048.0) / 4.0;
}

[numthreads(8, 8, 1)]
void CSFill(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    uint own = fillSplat.Load(int3(id.xy, 0));
    if (own != 0)
    {
        flowOut[id.xy] = float4(UnpackVector(own), 0, 0);
        return;
    }

    float2 result = float2(0, 0);
    bool found = false;
    [loop] for (int r = 1; r <= 6 && !found; r++)
    {
        float bestLen = 1e9;
        [loop] for (int y = -r; y <= r; y++)
        {
            [loop] for (int x = -r; x <= r; x++)
            {
                if (max(abs(x), abs(y)) != r) continue;
                int2 q = int2(id.xy) + int2(x, y);
                if (any(q < int2(0, 0)) || any(q >= gridSize)) continue;
                uint s = fillSplat.Load(int3(q, 0));
                if (s == 0) continue;
                float2 v = UnpackVector(s);
                float len = dot(v, v);
                if (len < bestLen)
                {
                    bestLen = len;
                    result = v;
                    found = true;
                }
            }
        }
    }
    flowOut[id.xy] = float4(result, 1, 0);
}

// ---------------------------------------------------------------------------
// 합성: 픽셀마다 주변 칸의 벡터와 "움직임 없음"을 후보로 놓고, 두 프레임이 잘 맞는 후보에 큰 비중을 준다.
// "움직임 없음" 후보 덕분에 움직이는 배경 위의 고정 UI가 뭉개지지 않는다.
// 한쪽 프레임에만 보이는 곳(가림)은 믿을 수 있는 쪽의 색을 쓰고, 어느 후보도 맞지 않으면 블렌딩한다.
// ---------------------------------------------------------------------------

Texture2D<float4> composeA      : register(t0);
Texture2D<float4> composeB      : register(t1);
Texture2D<float4> composeMotion : register(t2);
Texture2D<float4> composeF01    : register(t3);
Texture2D<float4> composeF10    : register(t4);
RWTexture2D<float4> colorOut    : register(u0);
Texture2D<uint>   composeStill  : register(t5);  // 아래 8비트: 픽셀이 이어서 같았던 쌍의 수, 8~23: 둘레가 움직인 비율 (0~65535), 24~31: 고정 UI 증거

float LumaOf(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
static const float kPopNccMin = 0.7;  // 판정 글자 처리(features 32)에서 앞뒤를 섞기 시작하는 제자리 상관

// 고정 픽셀: 게임 프레임 쌍마다 이어서 같았고 지금 쌍에서도 같은 픽셀은 원래 색을 그대로 둘 비율.
// 조준점이나 얇은 글자처럼 작은 고정 UI는 둘레의 배경이 움직여 PatchError가 커지고, 배경의 움직임 후보가 이겨 찢어졌다.
// 48쌍(30fps면 1.6초) 넘게 이어서 같고, 둘레(움직임 격자 3x3칸)가 거의 늘 움직였거나 거의 늘 멈춰 있었어야 켠다.
// 처음에는 "이어서 같음"만 봤더니 리듬게임의 어두운 레인이 고정으로 잡혀, 앞뒤 프레임 사이를 지나가는 노트가 생성 프레임에서
// 사라졌다 (자체 테스트 rhythm -2.7dB). 작고 빠른 노트는 한 프레임에 제 길이보다 멀리 가서 건너뛰는 픽셀이 있다.
// 그런 레인은 노트가 지날 때만 움직이므로 빠진다. 둘레가 오래 멈춰 있던 곳(큰 UI 판의 안쪽)도 지킨다:
// 물체가 다가오면 그 쌍의 움직임이 먼저 기록되어(합성보다 앞서 갱신) 비율이 0.125로 올라가 바로 풀린다.
// 고정 UI 증거(features 16): 둘레가 움직이는 쌍에서도 픽셀이 같았던 횟수 (달라지면 0으로). 128번(30fps 게임에서 화면이 4초 남짓 움직이는 동안)
// 넘게 그대로였으면 둘레 조건 없이 지킨다. 마인크래프트 체력 하트의 가장자리는 둘레가 움직였다 멈췄다 해서 비율이 0.05~0.7 사이에 머물러 빠졌다.
// 저장된 고정 UI 상자 안은 확대를 시작할 때 이 값을 160으로 채워 처음부터 지킨다 (다른 환경이라 실제로 바뀌는 픽셀은 한 번 바뀌면 0이 된다).
float StillKeep(int2 id, float3 a, float3 b)
{
    if ((features & 1) == 0) return 0;
    float3 d = abs(a - b);
    if (max(d.r, max(d.g, d.b)) >= 4.0 / 255.0) return 0;
    uint s = composeStill.Load(int3(id, 0));
    float moving = float((s >> 8) & 0xFFFF) / 65535.0;
    float keep = saturate((float(s & 0xFF) - 40.0) / 8.0) * max(saturate((moving - 0.7) / 0.1), saturate((0.05 - moving) / 0.03));
    if (features & 16) keep = max(keep, saturate((float(s >> 24) - 96.0) / 32.0));
    return keep;
}

float3 SampleA(float2 p) { return composeA.SampleLevel(linSamp, p / float2(srcSize), 0).rgb; }
float3 SampleB(float2 p) { return composeB.SampleLevel(linSamp, p / float2(srcSize), 0).rgb; }

// 앞뒤 프레임 조각의 정규화 상관 (밝기와 대비가 달라도 무늬가 같으면 1에 가깝다). 무늬가 없으면 0.
float Ncc(float2 p, float2 v)
{
    float sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float2 q = p + float2(x, y) * 2.0;
            float la = LumaOf(SampleA(q - t * v));
            float lb = LumaOf(SampleB(q + (1.0 - t) * v));
            sa += la;
            sb += lb;
            saa += la * la;
            sbb += lb * lb;
            sab += la * lb;
        }
    }
    sa /= 9.0;
    sb /= 9.0;
    float va = saa / 9.0 - sa * sa;
    float vb = sbb / 9.0 - sb * sb;
    const float minVar = (4.0 / 255.0) * (4.0 / 255.0);
    if (va < minVar || vb < minVar) return 0;
    return (sab / 9.0 - sa * sb) / sqrt(va * vb);
}

float PatchError(float2 p, float2 v)
{
    static const float2 offsets[5] = { float2(0, 0), float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2) };
    float sum = 0;
    [loop] for (int i = 0; i < 5; i++)
    {
        float2 q = p + offsets[i];
        sum += abs(LumaOf(SampleA(q - t * v)) - LumaOf(SampleB(q + (1.0 - t) * v)));
    }
    return sum / 5.0;
}

// 두 프레임의 색을 합친다. 외삽(t > 1)은 최신 프레임만 쓴다.
float3 Resolve(float2 p, float2 v, inout float err)
{
    float2 pa = p - t * v;
    float2 pb = p + (1.0 - t) * v;
    float3 ca = SampleA(pa);
    float3 cb = SampleB(pb);
    if (t > 1.0) return cb;

    // 한쪽이 화면 밖이면 그 프레임에는 없는 내용이므로 다른 쪽만 쓴다.
    bool outA = any(pa < float2(0, 0)) || any(pa >= float2(srcSize));
    bool outB = any(pb < float2(0, 0)) || any(pb >= float2(srcSize));
    if (outA != outB)
    {
        err = min(err, blendTol * 0.5);
        return outA ? cb : ca;
    }

    float wb = t;
    if (err > blendTol && dot(v, v) > 0.25)
    {
        // 맞지 않는 후보: 벡터가 어느 쪽 프레임에서 실제로 관측됐는지 본다.
        float tol = max(2.0, 0.25 * length(v));
        int2 qa = clamp(int2(pa / cellSrc), int2(0, 0), gridSize - 1);
        int2 qb = clamp(int2(pb / cellSrc), int2(0, 0), gridSize - 1);
        bool validA = length(composeF01.Load(int3(qa, 0)).xy - v) < tol;
        bool validB = length(composeF10.Load(int3(qb, 0)).xy + v) < tol;
        if (validA != validB)
        {
            wb = validB ? 1.0 : 0.0;
            err = blendTol;
        }
    }
    return lerp(ca, cb, wb);
}

[numthreads(8, 8, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    float2 p = float2(id.xy) + 0.5;
    float3 a0 = composeA.Load(int3(id.xy, 0)).rgb;
    float3 b0 = composeB.Load(int3(id.xy, 0)).rgb;
    float3 stillColor = t > 1.0 ? b0 : lerp(a0, b0, t);
    float keep = StillKeep(int2(id.xy), a0, b0);
    [branch] if (keep >= 1.0)
    {
        colorOut[id.xy] = float4(stillColor, 1);
        return;
    }
    float2 cellPos = p / cellSrc - 0.5;
    int2 base = int2(floor(cellPos));
    float2 f = cellPos - base;

    float3 sum = 0;
    float weightSum = 0;
    const float tol2 = max(blendTol * blendTol, 1e-6);
    // 주변 네 칸의 벡터는 같은 경우가 많다 (멈춘 곳, 함께 움직이는 배경). 같은 벡터는 결과도 같으므로
    // 앞에서 계산한 색과 오차를 그대로 쓴다. 결과는 매번 계산할 때와 같고 텍스처 읽기만 준다.
    float bestW = -1;
    int bestI = 0;
    float2 vs[4];
    float3 cs[4];
    float es[4];   // Resolve를 거친 오차
    float pes[4];  // PatchError 값 (움직임 없음 후보가 다시 쓴다)
    [unroll] for (int i = 0; i < 4; i++)
    {
        int2 o = int2(i & 1, i >> 1);
        int2 q = clamp(base + o, int2(0, 0), gridSize - 1);
        float2 v = composeMotion.Load(int3(q, 0)).xy;
        float2 w = lerp(1.0 - f, f, float2(o));
        float dist = w.x * w.y + 0.02;
        bool same = false;
        float3 c = 0;
        float err = 0, pe = 0;
        [unroll] for (int j = 0; j < i; j++)
        {
            if (!same && all(vs[j] == v))
            {
                same = true;
                c = cs[j];
                err = es[j];
                pe = pes[j];
            }
        }
        [branch] if (!same)
        {
            pe = PatchError(p, v);
            err = pe;
            c = Resolve(p, v, err);
        }
        vs[i] = v;
        cs[i] = c;
        es[i] = err;
        pes[i] = pe;
        float wgt = dist * exp(-err * err / tol2);
        sum += c * wgt;
        weightSum += wgt;
        if (wgt > bestW)
        {
            bestW = wgt;
            bestI = i;
        }
    }
    float zeroPe = 0;
    {
        // 움직임 없음 후보
        bool same = false;
        float pe = 0;
        [unroll] for (int j = 0; j < 4; j++)
        {
            if (!same && all(vs[j] == float2(0, 0)))
            {
                same = true;
                pe = pes[j];
            }
        }
        [branch] if (!same) pe = PatchError(p, float2(0, 0));
        zeroPe = pe;
        float err = pe * 0.7;
        float3 c = Resolve(p, float2(0, 0), err);
        float wgt = 0.25 * exp(-err * err / tol2);
        sum += c * wgt;
        weightSum += wgt;
    }

    float3 result;
    if (t > 1.0)
        result = weightSum > 1e-3 ? sum / weightSum : SampleB(p);
    else if (weightSum > 1e-3)
        result = sum / weightSum;
    else
        result = lerp(SampleA(p), SampleB(p), t);

    // 제자리에서 밝기나 대비만 바뀌는 곳 (BREAK 같은 판정 글자, 서서히 밝아지는 화면): 밝기 차이로는 "움직임 없음"이 맞지 않아
    // 엉뚱한 곳의 벡터가 이겨 찢어졌다. 무늬의 상관으로 다시 보고, 제자리 상관이 가장 잘 맞는 벡터보다 높으면 앞뒤를 섞는다.
    // 잘 맞는 후보가 있으면(평소의 움직임) 계산하지 않는다.
    // features 32: 판정 글자(BREAK)가 제자리에서 밝기만 바뀌는 쌍은 밝기 차이가 작아(blendTol 아래) 이 검사에 들지 않았고,
    // 글자 무늬 안에서 엉뚱하게 잘 맞는 벡터가 이겨 앞뒤를 섞은 것보다 4~5dB 나빴다. 그런 곳도 상관을 보되(넓힌 검사),
    // 매끈한 무늬가 움직이면 제자리와 움직임의 상관이 둘 다 1에 가까워 잘못 섞였으므로(화면 이동 -1.4dB) 제자리 쪽이 0.02 넘게 높아야 섞는다.
    const bool usual = zeroPe > blendTol && es[bestI] > blendTol * 0.5;
    const bool agree = all(vs[0] == vs[1]) && all(vs[0] == vs[2]) && all(vs[0] == vs[3]);
    const bool wide = !usual && !agree && (features & 32) && zeroPe > blendTol * 0.25 && zeroPe <= blendTol;
    [branch] if ((features & 4) && t <= 1.0 && (usual || wide))
    {
        float n0 = Ncc(p, float2(0, 0));
        const float n0Min = usual ? 0.9 : kPopNccMin;
        const float margin = usual ? -0.02 : 0.02;
        [branch] if (n0 > n0Min)
        {
            float nv = all(vs[bestI] == float2(0, 0)) ? n0 : Ncc(p, vs[bestI]);
            float k = saturate((n0 - n0Min) / 0.05) * saturate((n0 - nv - margin) / 0.03);
            result = lerp(result, lerp(a0, b0, t), k);
        }
    }
    colorOut[id.xy] = float4(lerp(result, stillColor, keep), 1);
}

// ---------------------------------------------------------------------------
// 단순 블렌딩: 비교 기준이자 움직임 정보가 없을 때의 대체 수단
// ---------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSBlend(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    float3 a = composeA.Load(int3(id.xy, 0)).rgb;
    float3 b = composeB.Load(int3(id.xy, 0)).rgb;
    colorOut[id.xy] = float4(lerp(a, b, saturate(t)), 1);
}

// ---------------------------------------------------------------------------
// 고정 픽셀 기록: 이웃한 게임 프레임 쌍마다 픽셀이 같았으면 1을 더하고 달랐으면 0으로 되돌린다 (아래 8비트, 최대 255).
// 둘레 3x3칸의 움직임 격자에 1픽셀 넘는 벡터가 있었는지를 지수 평균으로 쌓는다 (위 16비트, 1/8씩 따라감).
// ---------------------------------------------------------------------------

Texture2D<float4> stillFlow : register(t2);
RWTexture2D<uint> stillOut : register(u0);  // R32_UINT: 형식 있는 UAV에서 읽고 쓸 수 있는 것은 32비트 한 칸뿐이라 둘을 묶는다

[numthreads(8, 8, 1)]
void CSStill(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    float3 d = abs(composeA.Load(int3(id.xy, 0)).rgb - composeB.Load(int3(id.xy, 0)).rgb);
    bool same = max(d.r, max(d.g, d.b)) < 3.0 / 255.0;
    int2 cell = int2(id.xy) / cellSrc;
    float moving = 0;
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            int2 q = clamp(cell + int2(x, y), int2(0, 0), gridSize - 1);
            if (dot(stillFlow.Load(int3(q, 0)).xy, stillFlow.Load(int3(q, 0)).xy) > 1.0) moving = 1;
        }
    }
    uint s = stillOut[id.xy];
    uint count = same ? min((s & 0xFF) + 1, 255u) : 0;
    float share = lerp(float((s >> 8) & 0xFFFF) / 65535.0, moving, 0.125);
    // 고정 UI 증거는 무늬가 있는 픽셀(둘레 3x3의 밝기 차이 6/255 초과)에서만 쌓는다. 리듬게임의 빈 레인이나 하늘처럼 단색인 곳은
    // 오래 그대로여도 UI가 아니고, 지켜 봐야 얻는 것은 없이 사이를 지나가는 노트만 지워진다 (DJMAX 전체 -0.03dB).
    float lo = 1, hi = 0;
    [unroll] for (int ty = -1; ty <= 1; ty++)
    {
        [unroll] for (int tx = -1; tx <= 1; tx++)
        {
            float l = LumaOf(composeB.Load(int3(clamp(int2(id.xy) + int2(tx, ty), int2(0, 0), srcSize - 1), 0)).rgb);
            lo = min(lo, l);
            hi = max(hi, l);
        }
    }
    bool textured = hi - lo > 6.0 / 255.0;
    uint ui = same ? (moving > 0 && textured ? min((s >> 24) + 1, 255u) : (s >> 24)) : 0;
    stillOut[id.xy] = count | (uint(share * 65535.0 + 0.5) << 8) | (ui << 24);
}

// ---------------------------------------------------------------------------
// 고정 UI 상자 찾기: 8x8 픽셀 묶음마다 고정 UI 증거가 192 이상인 픽셀의 비율(x)과 둘레가 움직인 비율의 평균(y).
// CPU가 이 작은 격자를 읽어 상자로 묶는다 (5초마다).
// ---------------------------------------------------------------------------

Texture2D<uint> stillGridIn : register(t0);
RWTexture2D<float2> stillGridOut : register(u0);

[numthreads(8, 8, 1)]
void CSStillGrid(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    float ui = 0, moving = 0, n = 0;
    [loop] for (int y = 0; y < 8; y++)
    {
        [loop] for (int x = 0; x < 8; x++)
        {
            int2 q = int2(id.xy) * 8 + int2(x, y);
            if (any(q >= srcSize)) continue;
            uint s = stillGridIn.Load(int3(q, 0));
            ui += (s >> 24) >= 192 ? 1.0 : 0.0;
            moving += float((s >> 8) & 0xFFFF) / 65535.0;
            n += 1;
        }
    }
    stillGridOut[id.xy] = n > 0 ? float2(ui, moving) / n : float2(0, 0);
}

// ---------------------------------------------------------------------------
// 장면 전환 판단: 움직임을 믿을 수 없고(일치 검사 신뢰도 0.25 미만) 제자리 무늬도 다른(1/4 휘도의 상관 0.5 미만) 칸의 비율.
// 밝기가 크게 바뀌는 화면은 움직임을 못 믿어도 무늬가 같아서 빠진다. 그룹 하나가 격자 전체를 센다.
// 실제 게임 녹화를 이어 붙인 전환은 이 비율이 0.36~0.42에 그쳤다 (어두운 평면은 움직임 0으로 앞뒤가 맞아 믿을 만하게 나온다).
// 그래서 1/4 휘도의 밝기 분포(32칸) 차이도 본다. 실제 전환은 0.9 이상, 같은 장면은 0.18 이하였다. 0.6을 넘고 자리마다의 밝기 상관이 0.15 아래면 전환이다.
// ---------------------------------------------------------------------------

Texture2D<float4> cutFlow : register(t0);
Texture2D<float>  cutLumaA : register(t1);  // 1/4 휘도
Texture2D<float>  cutLumaB : register(t2);
RWTexture2D<float4> cutOut : register(u0);

bool SameStructure(int2 cell)
{
    float2 c = (float2(cell) + 0.5) * cellSrc / 4.0;
    float sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            int2 q = clamp(int2(c + float2(x, y) * 2.0), int2(0, 0), lvlSize - 1);
            float a = cutLumaA.Load(int3(q, 0));
            float b = cutLumaB.Load(int3(q, 0));
            sa += a;
            sb += b;
            saa += a * a;
            sbb += b * b;
            sab += a * b;
        }
    }
    sa /= 9.0;
    sb /= 9.0;
    float va = saa / 9.0 - sa * sa, vb = sbb / 9.0 - sb * sb;
    const float minVar = (4.0 / 255.0) * (4.0 / 255.0);
    if (va < minVar && vb < minVar) return true;   // 둘 다 무늬가 없으면 같은 장면으로 본다
    if (va < minVar || vb < minVar) return false;
    return (sab / 9.0 - sa * sb) / sqrt(va * vb) > 0.5;
}
groupshared float3 gsCut[256];  // 믿을 수 없는 칸, 칸 수, 평균 밝기 곱의 합
groupshared float4 gsCorr[256];  // 평균 밝기 a, b, a*a, b*b의 합
groupshared uint gsHistA[32];
groupshared uint gsHistB[32];

[numthreads(16, 16, 1)]
void CSCutStats(uint3 gt : SV_GroupThreadID)
{
    uint index = gt.y * 16 + gt.x;
    if (index < 32)
    {
        gsHistA[index] = 0;
        gsHistB[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop] for (int hy = (int)gt.y; hy < lvlSize.y; hy += 16)
    {
        [loop] for (int hx = (int)gt.x; hx < lvlSize.x; hx += 16)
        {
            InterlockedAdd(gsHistA[min(uint(saturate(cutLumaA.Load(int3(hx, hy, 0))) * 32.0), 31u)], 1);
            InterlockedAdd(gsHistB[min(uint(saturate(cutLumaB.Load(int3(hx, hy, 0))) * 32.0), 31u)], 1);
        }
    }
    float low = 0, n = 0, corrAB = 0;
    float4 corr = 0;
    [loop] for (int y = (int)gt.y; y < gridSize.y; y += 16)
    {
        [loop] for (int x = (int)gt.x; x < gridSize.x; x += 16)
        {
            if (cutFlow.Load(int3(x, y, 0)).w < 0.25 && !SameStructure(int2(x, y))) low += 1.0;
            n += 1.0;
            // 칸 둘레(1/4 휘도 6픽셀 너비)의 평균 밝기. 앞뒤 화면의 이 값들의 상관을 화면 전체로 구한다.
            const int2 q = clamp(int2((float2(x, y) + 0.5) * cellSrc / 4.0), int2(0, 0), lvlSize - 1);
            float ma = 0, mb = 0;
            [unroll] for (int ty = -1; ty <= 1; ty++)
            {
                [unroll] for (int tx = -1; tx <= 1; tx++)
                {
                    const int2 p = clamp(q + int2(tx, ty) * 2, int2(0, 0), lvlSize - 1);
                    ma += cutLumaA.Load(int3(p, 0));
                    mb += cutLumaB.Load(int3(p, 0));
                }
            }
            ma /= 9.0;
            mb /= 9.0;
            corr += float4(ma, mb, ma * ma, mb * mb);
            corrAB += ma * mb;
        }
    }
    gsCut[index] = float3(low, n, corrAB);
    gsCorr[index] = corr;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 128; step > 0; step >>= 1)
    {
        if (index < step)
        {
            gsCut[index] += gsCut[index + step];
            gsCorr[index] += gsCorr[index + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0)
    {
        float hist = 0;
        [unroll] for (uint i = 0; i < 32; i++) hist += abs(float(gsHistA[i]) - float(gsHistB[i]));
        hist *= 0.5 / max(float(lvlSize.x * lvlSize.y), 1.0);
        const float cells = max(gsCut[0].y, 1.0);
        const float lowShare = gsCut[0].x / cells;
        // 화면 전체가 밝아지거나 어두워져도 밝기 분포는 크게 바뀐다. 그때는 자리마다의 밝기가 앞 화면을 따라가므로(상관이 높음)
        // 상관이 0.15 아래일 때만 전환으로 본다. 실제 녹화에서 전환은 -0.06~0, 서서히 바뀌는 전환은 0.56 이상, 같은 장면은 0.23 이상이었다.
        const float4 m = gsCorr[0] / cells;
        const float va = m.z - m.x * m.x, vb = m.w - m.y * m.y;
        const float r = (va > 1e-6 && vb > 1e-6) ? (gsCut[0].z / cells - m.x * m.y) / sqrt(va * vb) : 1.0;
        // x: 판단에 쓰는 점수 (기준 0.45), y: 믿을 수 없는 칸 비율, z: 밝기 분포 차이, w: 칸 평균 밝기의 상관
        // 캡처가 한두 장씩 빠져 간격이 50~67ms가 되면 빠르게 도는 장면의 믿을 수 없는 칸 비율이 0.44까지 올라 전환으로 잘못 보았다.
        // 같은 장면이면 칸 평균 밝기의 상관이 0.95 정도로 남으므로, 상관이 0.5를 넘으면 전환으로 보지 않는다.
        cutOut[int2(0, 0)] = float4(min(max(lowShare, min(hist * 0.75, (0.3 - r) * 3.0)), (0.65 - r) * 3.0), lowShare, hist, r);
    }
}

// ---------------------------------------------------------------------------
// RIFE: 신경망 입력 크기로 줄이고, 신경망이 준 움직임으로 원본 해상도에서 합성한다.
// 신경망은 시점 0.5의 움직임 하나만 구하고, 다른 시점은 움직임을 비례해서 쓴다 (근사).
// ---------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSResize(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)lvlSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(lvlSize);
    colorOut[id.xy] = float4(composeA.SampleLevel(linSamp, uv, 0).rgb, 1);
}

Texture2D<float> rifePlanes : register(t2);  // 평면 5장을 세로로 이은 것: 움직임 x0, y0, x1, y1, 마스크

float4 RifeFlowAt(int2 c)
{
    return float4(rifePlanes.Load(int3(c.x, c.y, 0)), rifePlanes.Load(int3(c.x, c.y + lvlSize.y, 0)),
                  rifePlanes.Load(int3(c.x, c.y + lvlSize.y * 2, 0)), rifePlanes.Load(int3(c.x, c.y + lvlSize.y * 3, 0)));
}

float RifeMaskAt(int2 c)
{
    return rifePlanes.Load(int3(c.x, c.y + lvlSize.y * 4, 0));
}

[numthreads(8, 8, 1)]
void CSRifeCompose(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    float2 p = float2(id.xy) + 0.5;
    float2 q = p * float2(lvlSize) / float2(srcSize) - 0.5;
    int2 b0 = clamp(int2(floor(q)), int2(0, 0), lvlSize - 1);
    int2 b1 = min(b0 + int2(1, 1), lvlSize - 1);
    float2 f = saturate(q - float2(b0));

    // 부동소수 텍스처는 선형 필터를 보장하지 않으므로 직접 섞는다.
    float4 flow = lerp(lerp(RifeFlowAt(b0), RifeFlowAt(int2(b1.x, b0.y)), f.x), lerp(RifeFlowAt(int2(b0.x, b1.y)), RifeFlowAt(b1), f.x), f.y);
    float mask = lerp(lerp(RifeMaskAt(b0), RifeMaskAt(int2(b1.x, b0.y)), f.x), lerp(RifeMaskAt(int2(b0.x, b1.y)), RifeMaskAt(b1), f.x), f.y);

    float2 scale = float2(srcSize) / float2(lvlSize);
    float3 ca = SampleA(p + flow.xy * scale * 2.0 * t);
    float3 cb = SampleB(p + flow.zw * scale * 2.0 * (1.0 - t));
    float wa = mask * (1.0 - t);
    float wb = (1.0 - mask) * t;
    float3 pa = composeA.Load(int3(id.xy, 0)).rgb;
    float3 pb = composeB.Load(int3(id.xy, 0)).rgb;
    float3 result = (ca * wa + cb * wb) / max(wa + wb, 1e-4);
    colorOut[id.xy] = float4(lerp(result, lerp(pa, pb, t), StillKeep(int2(id.xy), pa, pb)), 1);
}

// ---------------------------------------------------------------------------
// 움직임 시각화: 방향을 색상으로, 크기를 진하기로 표시하고 원본 밝기를 살짝 비춘다.
// ---------------------------------------------------------------------------

float3 HueToRgb(float h)
{
    return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

[numthreads(8, 8, 1)]
void CSVisualize(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    int2 q = clamp(int2(id.xy) / cellSrc, int2(0, 0), gridSize - 1);
    float2 v = composeMotion.Load(int3(q, 0)).xy;
    float mag = saturate(length(v) / 32.0);
    float hue = atan2(v.y, v.x) / 6.2831853 + 0.5;
    float luma = LumaOf(composeB.Load(int3(id.xy, 0)).rgb);
    colorOut[id.xy] = float4(lerp(float3(luma, luma, luma) * 0.5, HueToRgb(hue), mag), 1);
}

// ---------------------------------------------------------------------------
// 오차 측정: 8x8 묶음마다 평균 제곱 오차를 적는다. 작은 결과만 읽어 CPU에서 평균 낸다.
// ---------------------------------------------------------------------------

Texture2D<float4> mseX : register(t0);
Texture2D<float4> mseY : register(t1);
RWTexture2D<float> mseOut : register(u0);

[numthreads(4, 4, 1)]
void CSMse(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)gridSize)) return;
    float sum = 0;
    int count = 0;
    [loop] for (int y = 0; y < 8; y++)
    {
        [loop] for (int x = 0; x < 8; x++)
        {
            int2 p = int2(id.xy) * 8 + int2(x, y);
            if (any(p >= srcSize)) continue;
            float3 d = mseX.Load(int3(p, 0)).rgb - mseY.Load(int3(p, 0)).rgb;
            sum += dot(d, d) / 3.0;
            count++;
        }
    }
    mseOut[id.xy] = count > 0 ? sum / count : 0;
}

// ---------------------------------------------------------------------------
// 영역 붙이기: 프레임 생성 영역(gridSize 위치, lvlSize 크기)의 결과를 화면 전체 게임 프레임 위에 붙인다.
// ---------------------------------------------------------------------------

Texture2D<float4> pasteFull : register(t0);
Texture2D<float4> pasteRegion : register(t1);

[numthreads(8, 8, 1)]
void CSPaste(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)srcSize)) return;
    int2 p = int2(id.xy) - gridSize;
    bool inside = all(p >= 0) && all(p < lvlSize);
    colorOut[id.xy] = inside ? pasteRegion.Load(int3(p, 0)) : pasteFull.Load(int3(id.xy, 0));
}
)";
