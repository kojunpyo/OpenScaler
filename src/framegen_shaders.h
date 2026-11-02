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
    int3  padding;
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

        [loop] for (int y = -1; y <= 1; y++)
        {
            [loop] for (int x = -1; x <= 1; x++)
            {
                int2 q = clamp(pc + int2(x, y), int2(0, 0), prevGridSize - 1);
                TRY_CANDIDATE(int2(round(matchPrev.Load(int3(q, 0)).xy / levelScale)));
            }
        }
        TRY_CANDIDATE(int2(0, 0));

        if (temporalCell > 0)
        {
            int2 tc = clamp(srcC / temporalCell, int2(0, 0), temporalGrid - 1);
            TRY_CANDIDATE(int2(round(matchTemporal.Load(int3(tc, 0)).xy / levelScale)));
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

float LumaOf(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 SampleA(float2 p) { return composeA.SampleLevel(linSamp, p / float2(srcSize), 0).rgb; }
float3 SampleB(float2 p) { return composeB.SampleLevel(linSamp, p / float2(srcSize), 0).rgb; }

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
    float2 cellPos = p / cellSrc - 0.5;
    int2 base = int2(floor(cellPos));
    float2 f = cellPos - base;

    float3 sum = 0;
    float weightSum = 0;
    const float tol2 = max(blendTol * blendTol, 1e-6);
    [loop] for (int i = 0; i < 5; i++)
    {
        float2 v;
        float dist;
        float err;
        if (i < 4)
        {
            int2 o = int2(i & 1, i >> 1);
            int2 q = clamp(base + o, int2(0, 0), gridSize - 1);
            v = composeMotion.Load(int3(q, 0)).xy;
            float2 w = lerp(1.0 - f, f, float2(o));
            dist = w.x * w.y + 0.02;
            err = PatchError(p, v);
        }
        else
        {
            v = float2(0, 0);
            dist = 0.25;
            err = PatchError(p, v) * 0.7;
        }
        float3 c = Resolve(p, v, err);
        float w = dist * exp(-err * err / tol2);
        sum += c * w;
        weightSum += w;
    }

    float3 result;
    if (t > 1.0)
        result = weightSum > 1e-3 ? sum / weightSum : SampleB(p);
    else if (weightSum > 1e-3)
        result = sum / weightSum;
    else
        result = lerp(SampleA(p), SampleB(p), t);
    colorOut[id.xy] = float4(result, 1);
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

Texture2D<float4> rifeFlow : register(t2);
Texture2D<float>  rifeMask : register(t3);

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
    float4 flow = lerp(lerp(rifeFlow.Load(int3(b0.x, b0.y, 0)), rifeFlow.Load(int3(b1.x, b0.y, 0)), f.x),
                       lerp(rifeFlow.Load(int3(b0.x, b1.y, 0)), rifeFlow.Load(int3(b1.x, b1.y, 0)), f.x), f.y);
    float mask = lerp(lerp(rifeMask.Load(int3(b0.x, b0.y, 0)), rifeMask.Load(int3(b1.x, b0.y, 0)), f.x),
                      lerp(rifeMask.Load(int3(b0.x, b1.y, 0)), rifeMask.Load(int3(b1.x, b1.y, 0)), f.x), f.y);

    float2 scale = float2(srcSize) / float2(lvlSize);
    float3 ca = SampleA(p + flow.xy * scale * 2.0 * t);
    float3 cb = SampleB(p + flow.zw * scale * 2.0 * (1.0 - t));
    float wa = mask * (1.0 - t);
    float wb = (1.0 - mask) * t;
    colorOut[id.xy] = float4((ca * wa + cb * wb) / max(wa + wb, 1e-4), 1);
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
)";
