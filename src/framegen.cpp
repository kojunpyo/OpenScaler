#include "framegen.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "framegen_shaders.h"

using winrt::check_hresult;

namespace {

const double kPauseGap = 0.25;       // 프레임 간격이 이보다 길면 게임이 멈췄던 것으로 보고 보간하지 않는다
const double kEvalPeriod = 1.0;      // 채점 주기 (초)
const double kSamplePeriod = 0.25;   // 화면 지도 표본 주기 (초)
const double kDisplayMargin = 0.002; // 프레임 도착 지연에 대비해 표시를 조금 더 늦춘다 (초)
const float kMaxExtrapolate = 1.75f; // 외삽은 프레임 간격의 75%까지만 앞서 나간다
const float kZeroBias = 0.002f;
const int kWideSearchLevel = 3;      // 1/8 영상에서 넓게 찾는다
const int kWideSearchRadius = 16;    // 원본 기준 약 128픽셀
const double kRifeBudget = 0.7;      // 신경망이 프레임 간격의 이 비율을 넘으면 입력 해상도를 낮춘다
const double kSlowMoWarmup = 0.25;   // 슬로모션 녹화 직후 드라이버가 자리를 잡는 동안의 프레임은 쓰지 않는다 (초)
const float kDuplicateMse = 2e-7f;   // 1/8 영상의 한 묶음이 이보다 달라야 새 프레임으로 본다 (8비트 반올림 오차 수준)

float PsnrFromMse(float mse)
{
    return mse <= 1e-10f ? 100.0f : -10.0f * std::log10(mse);
}

}  // namespace

// framegen_shaders.h의 FgCB와 같은 레이아웃
struct FrameGen::Cb {
    int srcSize[2];
    int gridSize[2];
    int lvlSize[2];
    int prevGridSize[2];
    int cellLvl;
    int levelScale;
    int prevCellSrc;
    int cellSrc;
    int searchRadius;
    int coarsest;
    int finalPass;
    int hasHint;
    float smoothness;
    float t;
    float blendTol;
    float hintScale;
    int temporalGrid[2];
    int temporalCell;
    float zeroBias;
    int wideRadius;
    int padding[3];
};

com_ptr<ID3D11ComputeShader> FrameGen::CreateCs(const char* entry)
{
    com_ptr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kFrameGenShaderSource, sizeof(kFrameGenShaderSource) - 1, "framegen.hlsl", nullptr, nullptr, entry,
                            "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        if (errors) fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        check_hresult(hr);
    }
    com_ptr<ID3D11ComputeShader> cs;
    check_hresult(m_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.put()));
    return cs;
}

FrameGen::FrameGen(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device.copy_from(device);
    m_ctx.copy_from(context);

    m_csLuma = CreateCs("CSLuma");
    m_csDown = CreateCs("CSDown");
    m_csMatch = CreateCs("CSMatch");
    m_csConsist = CreateCs("CSConsist");
    m_csProject = CreateCs("CSProject");
    m_csFill = CreateCs("CSFill");
    m_csCompose = CreateCs("CSCompose");
    m_csBlend = CreateCs("CSBlend");
    m_csVisualize = CreateCs("CSVisualize");
    m_csMse = CreateCs("CSMse");
    m_csResize = CreateCs("CSResize");
    m_csRifeCompose = CreateCs("CSRifeCompose");

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Cb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hresult(m_device->CreateBuffer(&bd, nullptr, m_cb.put()));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(m_device->CreateSamplerState(&sd, m_sampler.put()));

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kMapW;
    td.Height = kMapH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check_hresult(m_device->CreateTexture2D(&td, nullptr, m_mapTex.put()));
    check_hresult(m_device->CreateShaderResourceView(m_mapTex.get(), nullptr, m_mapSrv.put()));

    auto makeTimer = [&](GpuTimer& timer) {
        D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
        for (auto& slot : timer.slots) {
            check_hresult(m_device->CreateQuery(&disjoint, slot.disjoint.put()));
            check_hresult(m_device->CreateQuery(&stamp, slot.begin.put()));
            check_hresult(m_device->CreateQuery(&stamp, slot.end.put()));
        }
    };
    makeTimer(m_timerEstimate);
    makeTimer(m_timerSynth);
}

void FrameGen::Reset()
{
    m_count = 0;
    m_pairValid = m_tripleValid = false;
    m_intervals.clear();
    m_interval = 0;
    m_held = nullptr;
    m_liveFlow = m_olderFlow = nullptr;
    m_pending.clear();
    m_evalCurrentGroup = -1;
    m_clock = m_lastRender = 0;
    m_rifePrevious.clear();
    for (auto& result : m_rifeResults) result.seq = -1;
    for (auto& set : m_flowSets) set.hasHistory = false;
}

FrameGen::Target FrameGen::MakeTarget(UINT w, UINT h, DXGI_FORMAT format, bool uav)
{
    Target target;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    check_hresult(m_device->CreateTexture2D(&td, nullptr, target.tex.put()));
    check_hresult(m_device->CreateShaderResourceView(target.tex.get(), nullptr, target.srv.put()));
    if (uav) check_hresult(m_device->CreateUnorderedAccessView(target.tex.get(), nullptr, target.uav.put()));
    return target;
}

void FrameGen::EnsureFrames(UINT w, UINT h)
{
    if (m_w == w && m_h == h && m_frames[0].color) return;
    Reset();
    m_w = w;
    m_h = h;
    m_flowSets.clear();
    m_flowSets.reserve(8);  // 포인터가 바뀌지 않게 미리 잡아 둔다 (칸 크기는 3가지뿐)

    for (int i = 0; i < kLevels; i++) {
        m_lvlW[i] = std::max(1u, w >> i);
        m_lvlH[i] = std::max(1u, h >> i);
    }
    for (Frame& f : m_frames) {
        Target color = MakeTarget(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false);
        f.color = color.tex;
        f.colorSrv = color.srv;
        for (int i = 0; i < kLevels; i++) {
            Target luma = MakeTarget(m_lvlW[i], m_lvlH[i], DXGI_FORMAT_R8_UNORM, true);
            f.lumaSrv[i] = luma.srv;
            f.lumaUav[i] = luma.uav;
        }
    }
    // 합성 결과. UAV는 BGRA를 지원하지 않지만 셰이더에서는 채널 순서가 같게 보인다.
    m_output = MakeTarget(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    m_evalOutput = MakeTarget(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    m_mse = MakeTarget((w + 7) / 8, (h + 7) / 8, DXGI_FORMAT_R32_FLOAT, true);
    m_duplicateMse = MakeTarget((m_lvlW[kDuplicateLevel] + 7) / 8, (m_lvlH[kDuplicateLevel] + 7) / 8, DXGI_FORMAT_R32_FLOAT, true);
    m_stagingPool.clear();
}

FrameGen::FlowSet& FrameGen::GetFlowSet(int cellPx)
{
    for (auto& set : m_flowSets)
        if (set.cellPx == cellPx) return set;

    FlowSet set;
    set.cellPx = cellPx;
    set.gridW = (m_w + cellPx - 1) / cellPx;
    set.gridH = (m_h + cellPx - 1) / cellPx;
    const DXGI_FORMAT flowFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (int level = 0; level < kLevels; level++) {
        int cellSrc = std::max(cellPx, 4 << level);
        set.pass[level] = MakeTarget((m_w + cellSrc - 1) / cellSrc, (m_h + cellSrc - 1) / cellSrc, flowFormat, true);
    }
    for (int i = 0; i < 2; i++) {
        set.raw[i] = MakeTarget(set.gridW, set.gridH, flowFormat, true);
        set.flow[i] = MakeTarget(set.gridW, set.gridH, flowFormat, true);
        set.older[i] = MakeTarget(set.gridW, set.gridH, flowFormat, false);
    }
    set.splat = MakeTarget(set.gridW, set.gridH, DXGI_FORMAT_R32_UINT, true);
    set.motion = MakeTarget(set.gridW, set.gridH, flowFormat, true);
    m_flowSets.push_back(std::move(set));
    return m_flowSets.back();
}

FrameGen::Cb FrameGen::BaseCb(const FlowSet* set) const
{
    Cb cb = {};
    cb.srcSize[0] = (int)m_w;
    cb.srcSize[1] = (int)m_h;
    if (set) {
        cb.gridSize[0] = (int)set->gridW;
        cb.gridSize[1] = (int)set->gridH;
        cb.cellSrc = set->cellPx;
    }
    cb.zeroBias = kZeroBias;
    return cb;
}

void FrameGen::Run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
                   const Cb& cb, UINT groupsX, UINT groupsY)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &cb, sizeof(cb));
    m_ctx->Unmap(m_cb.get(), 0);

    ID3D11Buffer* buffer = m_cb.get();
    ID3D11SamplerState* sampler = m_sampler.get();
    m_ctx->CSSetShader(cs, nullptr, 0);
    m_ctx->CSSetConstantBuffers(0, 1, &buffer);
    m_ctx->CSSetSamplers(0, 1, &sampler);
    m_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    m_ctx->CSSetShaderResources(0, (UINT)srvs.size(), srvs.begin());
    m_ctx->Dispatch(groupsX, groupsY, 1);

    // 다음 단계에서 같은 텍스처를 읽을 수 있도록 바로 풀어 준다.
    ID3D11ShaderResourceView* nullSrvs[6] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    m_ctx->CSSetShaderResources(0, (UINT)srvs.size(), nullSrvs);
    m_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

void FrameGen::BuildPyramid(Frame& f)
{
    Cb cb = BaseCb(nullptr);
    cb.lvlSize[0] = (int)m_lvlW[0];
    cb.lvlSize[1] = (int)m_lvlH[0];
    Run(m_csLuma.get(), { f.colorSrv.get() }, f.lumaUav[0].get(), cb, (m_lvlW[0] + 7) / 8, (m_lvlH[0] + 7) / 8);
    for (int i = 1; i < kLevels; i++) {
        cb.lvlSize[0] = (int)m_lvlW[i];
        cb.lvlSize[1] = (int)m_lvlH[i];
        // t0은 비워 두고 t1에 윗단계를 넣는다
        Run(m_csDown.get(), { nullptr, f.lumaSrv[i - 1].get() }, f.lumaUav[i].get(), cb, (m_lvlW[i] + 7) / 8, (m_lvlH[i] + 7) / 8);
    }
}

void FrameGen::Estimate(const Frame& a, const Frame& b, const FgParams& params, FlowSet& set, bool temporal, bool useMap, double interval)
{
    const int cellPx = params.cellPx;
    for (int dir = 0; dir < 2; dir++) {
        const Frame& ref = dir == 0 ? a : b;
        const Frame& tgt = dir == 0 ? b : a;

        int prevCell = 0;
        ID3D11ShaderResourceView* prevSrv = nullptr;
        int prevGrid[2] = { 0, 0 };
        for (int level = kLevels - 1; level >= 0; level--) {
            const int cellSrc = std::max(cellPx, 4 << level);
            Cb cb = BaseCb(nullptr);
            cb.gridSize[0] = (int)((m_w + cellSrc - 1) / cellSrc);
            cb.gridSize[1] = (int)((m_h + cellSrc - 1) / cellSrc);
            cb.lvlSize[0] = (int)m_lvlW[level];
            cb.lvlSize[1] = (int)m_lvlH[level];
            cb.prevGridSize[0] = prevGrid[0];
            cb.prevGridSize[1] = prevGrid[1];
            cb.cellLvl = cellSrc >> level;
            cb.levelScale = 1 << level;
            cb.prevCellSrc = prevCell;
            cb.cellSrc = cellSrc;
            cb.searchRadius = params.coarseRadius;
            cb.coarsest = level == kLevels - 1;
            cb.finalPass = level == 0;
            cb.hasHint = useMap;
            cb.wideRadius = level == kWideSearchLevel ? kWideSearchRadius : 0;
            cb.smoothness = params.smoothness;
            cb.hintScale = (float)(dir == 0 ? interval : -interval);
            ID3D11ShaderResourceView* temporalSrv = nullptr;
            if (temporal && set.hasHistory) {
                temporalSrv = set.flow[dir].srv.get();
                cb.temporalGrid[0] = (int)set.gridW;
                cb.temporalGrid[1] = (int)set.gridH;
                cb.temporalCell = cellPx;
            }

            const Target& out = level == 0 ? set.raw[dir] : set.pass[level];
            Run(m_csMatch.get(),
                { ref.lumaSrv[level].get(), tgt.lumaSrv[level].get(), prevSrv, temporalSrv, useMap ? m_mapSrv.get() : nullptr },
                out.uav.get(), cb, (cb.gridSize[0] + 3) / 4, (cb.gridSize[1] + 3) / 4);

            prevSrv = out.srv.get();
            prevCell = cellSrc;
            prevGrid[0] = cb.gridSize[0];
            prevGrid[1] = cb.gridSize[1];
        }
    }

    for (int dir = 0; dir < 2; dir++) {
        Cb cb = BaseCb(&set);
        Run(m_csConsist.get(), { set.raw[dir].srv.get(), set.raw[1 - dir].srv.get() }, set.flow[dir].uav.get(), cb,
            (set.gridW + 7) / 8, (set.gridH + 7) / 8);
    }
    set.hasHistory = true;
}

void FrameGen::Project(const FlowSet& set, float t)
{
    const UINT zero[4] = {};
    m_ctx->ClearUnorderedAccessViewUint(set.splat.uav.get(), zero);

    Cb cb = BaseCb(&set);
    cb.t = t;
    Run(m_csProject.get(), { set.flow[0].srv.get(), set.flow[1].srv.get() }, set.splat.uav.get(), cb, (set.gridW + 7) / 8,
        (set.gridH + 7) / 8);
    Run(m_csFill.get(), { set.splat.srv.get() }, set.motion.uav.get(), cb, (set.gridW + 7) / 8, (set.gridH + 7) / 8);
}

void FrameGen::Synthesize(const Frame& a, const Frame& b, const FlowSet& set, float t, const FgParams& params, ID3D11UnorderedAccessView* out)
{
    Project(set, t);
    Cb cb = BaseCb(&set);
    cb.t = t;
    cb.blendTol = params.blendTol;
    Run(m_csCompose.get(), { a.colorSrv.get(), b.colorSrv.get(), set.motion.srv.get(), set.flow[0].srv.get(), set.flow[1].srv.get() },
        out, cb, (m_w + 7) / 8, (m_h + 7) / 8);
}

void FrameGen::Blend(const Frame& a, const Frame& b, float t, ID3D11UnorderedAccessView* out)
{
    Cb cb = BaseCb(nullptr);
    cb.t = t;
    Run(m_csBlend.get(), { a.colorSrv.get(), b.colorSrv.get() }, out, cb, (m_w + 7) / 8, (m_h + 7) / 8);
}

com_ptr<ID3D11Texture2D> FrameGen::GetStaging(UINT w, UINT h, DXGI_FORMAT format)
{
    for (size_t i = 0; i < m_stagingPool.size(); i++) {
        D3D11_TEXTURE2D_DESC td;
        m_stagingPool[i]->GetDesc(&td);
        if (td.Width == w && td.Height == h && td.Format == format) {
            auto tex = m_stagingPool[i];
            m_stagingPool.erase(m_stagingPool.begin() + i);
            return tex;
        }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> tex;
    check_hresult(m_device->CreateTexture2D(&td, nullptr, tex.put()));
    return tex;
}

void FrameGen::RecycleStaging(com_ptr<ID3D11Texture2D> tex)
{
    if (m_stagingPool.size() < 12) m_stagingPool.push_back(std::move(tex));
}

void FrameGen::QueueMse(ID3D11ShaderResourceView* x, ID3D11ShaderResourceView* y, int group, int index)
{
    D3D11_TEXTURE2D_DESC td;
    m_mse.tex->GetDesc(&td);
    Cb cb = BaseCb(nullptr);
    cb.gridSize[0] = (int)td.Width;
    cb.gridSize[1] = (int)td.Height;
    Run(m_csMse.get(), { x, y }, m_mse.uav.get(), cb, (td.Width + 3) / 4, (td.Height + 3) / 4);

    Pending p;
    p.staging = GetStaging(td.Width, td.Height, td.Format);
    m_ctx->CopyResource(p.staging.get(), m_mse.tex.get());
    p.kind = 0;
    p.evalGroup = group;
    p.evalIndex = index;
    p.w = td.Width;
    p.h = td.Height;
    m_pending.push_back(std::move(p));
}

float FrameGen::MseFromStaging(ID3D11Texture2D* staging, UINT w, UINT h)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    double sum = 0;
    for (UINT y = 0; y < h; y++) {
        auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + (size_t)y * mapped.RowPitch);
        for (UINT x = 0; x < w; x++) sum += row[x];
    }
    m_ctx->Unmap(staging, 0);
    return (float)(sum / ((size_t)w * h));
}

FgParams FrameGen::Effective(const FgParams& params, const FgOptions& options) const
{
    FgParams p = params;
    if (options.quality == FG_QUALITY_FAST) p.cellPx = std::max(p.cellPx, 8);
    else if (options.quality == FG_QUALITY_HIGH) {
        p.cellPx = 4;
        p.coarseRadius = std::max(p.coarseRadius, 16);
    }
    return p;
}

void FrameGen::UpdateMapTexture()
{
    if (!m_profile || m_mapUploaded == m_profile->mapSamples) return;
    m_mapUploaded = m_profile->mapSamples;
    std::vector<float> data((size_t)kMapW * kMapH * 4);
    for (size_t i = 0; i < m_profile->map.size(); i++) {
        const MapCell& m = m_profile->map[i];
        // 움직이는 일이 드문 칸은 후보를 내지 않는다 (0 후보와 같아진다)
        bool useful = m.activity > 0.1f && m.coherence > 0.4f;
        data[i * 4] = useful ? m.vx : 0;
        data[i * 4 + 1] = useful ? m.vy : 0;
        data[i * 4 + 2] = m.stillness;
        data[i * 4 + 3] = m.activity;
    }
    m_ctx->UpdateSubresource(m_mapTex.get(), 0, nullptr, data.data(), kMapW * 4 * sizeof(float), 0);
}

void FrameGen::RunEvaluation(const FgOptions& options)
{
    if (!m_profile) return;
    // 가운데 프레임을 가리고 양옆 프레임으로 만들어 본다. 간격이 2배이므로 절대 점수보다 비교에 쓴다.
    const Frame& f2 = m_frames[0];
    const Frame& f1 = m_frames[1];
    const Frame& f0 = m_frames[2];
    const double gap = f2.time - f0.time;
    const bool mapReady = options.useMap && m_profile->mapSamples >= 20;

    m_evalCurrentGroup = ++m_evalGroup;
    m_evalFilled = 0;

    const FgParams evalParams[2] = { Effective(m_profile->params, options), Effective(m_profile->Candidate(), options) };
    for (int i = 0; i < 2; i++) {
        FlowSet& set = GetFlowSet(evalParams[i].cellPx);
        // 실시간 움직임과 같은 격자를 쓰면 다음 프레임의 합성이 어긋나므로, 채점 뒤 다시 계산한다.
        Estimate(f0, f2, evalParams[i], set, false, mapReady && evalParams[i].useMap, gap);
        Synthesize(f0, f2, set, 0.5f, evalParams[i], m_evalOutput.uav.get());
        QueueMse(m_evalOutput.srv.get(), f1.colorSrv.get(), m_evalCurrentGroup, i);
    }
    Blend(f0, f2, 0.5f, m_evalOutput.uav.get());
    QueueMse(m_evalOutput.srv.get(), f1.colorSrv.get(), m_evalCurrentGroup, 2);
}

void FrameGen::QueueFlowSample(const FlowSet& set)
{
    Pending p;
    p.staging = GetStaging(set.gridW, set.gridH, DXGI_FORMAT_R32G32B32A32_FLOAT);
    m_ctx->CopyResource(p.staging.get(), set.flow[0].tex.get());
    p.kind = 1;
    p.w = set.gridW;
    p.h = set.gridH;
    p.interval = m_interval;
    m_pending.push_back(std::move(p));
}

void FrameGen::PollReadbacks()
{
    while (!m_pending.empty()) {
        Pending& p = m_pending.front();
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_ctx->Map(p.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        check_hresult(hr);

        if (p.kind == 0) {
            double sum = 0;
            for (UINT y = 0; y < p.h; y++) {
                auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + (size_t)y * mapped.RowPitch);
                for (UINT x = 0; x < p.w; x++) sum += row[x];
            }
            float mse = (float)(sum / ((size_t)p.w * p.h));
            if (p.evalGroup == m_evalCurrentGroup) {
                m_evalScores[p.evalIndex] = PsnrFromMse(mse);
                if (++m_evalFilled == 3 && m_profile) m_profile->AddEvaluation(m_evalScores[0], m_evalScores[1], m_evalScores[2]);
            }
        } else if (m_profile) {
            m_profile->AddFlowSample(static_cast<const float*>(mapped.pData), (int)p.w, (int)p.h,
                                     (int)(mapped.RowPitch / sizeof(float)), p.interval);
        }
        m_ctx->Unmap(p.staging.get(), 0);
        RecycleStaging(std::move(p.staging));
        m_pending.pop_front();
    }
}

void FrameGen::TimerBegin(GpuTimer& timer)
{
    TimerPoll(timer);
    auto& slot = timer.slots[timer.next];
    if (slot.pending) {
        timer.active = -1;  // 아직 결과가 안 나온 칸이 있으면 이번에는 재지 않는다
        return;
    }
    timer.active = timer.next;
    timer.next = (timer.next + 1) % (int)timer.slots.size();
    m_ctx->Begin(slot.disjoint.get());
    m_ctx->End(slot.begin.get());
}

void FrameGen::TimerEnd(GpuTimer& timer)
{
    if (timer.active < 0) return;
    auto& slot = timer.slots[timer.active];
    m_ctx->End(slot.end.get());
    m_ctx->End(slot.disjoint.get());
    slot.pending = true;
    timer.active = -1;
}

void FrameGen::TimerPoll(GpuTimer& timer)
{
    for (auto& slot : timer.slots) {
        if (!slot.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info;
        UINT64 begin, end;
        if (m_ctx->GetData(slot.disjoint.get(), &info, sizeof(info), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (m_ctx->GetData(slot.begin.get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (m_ctx->GetData(slot.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        slot.pending = false;
        if (!info.Disjoint && info.Frequency) {
            double ms = double(end - begin) * 1000.0 / double(info.Frequency);
            timer.ms = timer.ms <= 0 ? ms : timer.ms * 0.9 + ms * 0.1;
        }
    }
}

// 게임이 그리지 않았는데도 캡처 프레임이 올 수 있다 (30fps 게임에서 60번씩).
// 이것을 진짜 프레임으로 세면 프레임 간격이 틀어지고 멈춘 프레임 사이를 보간하게 되므로 걸러 낸다.
// 작은 결과 하나만 기다려 읽는다. 이 판단이 늦으면 다음 프레임까지 밀리므로 비동기로 하지 않는다.
bool FrameGen::IsDuplicate(const Frame& a, const Frame& b)
{
    D3D11_TEXTURE2D_DESC td;
    m_duplicateMse.tex->GetDesc(&td);
    Cb cb = BaseCb(nullptr);
    cb.srcSize[0] = (int)m_lvlW[kDuplicateLevel];
    cb.srcSize[1] = (int)m_lvlH[kDuplicateLevel];
    cb.gridSize[0] = (int)td.Width;
    cb.gridSize[1] = (int)td.Height;
    Run(m_csMse.get(), { a.lumaSrv[kDuplicateLevel].get(), b.lumaSrv[kDuplicateLevel].get() }, m_duplicateMse.uav.get(), cb,
        (td.Width + 3) / 4, (td.Height + 3) / 4);

    auto staging = GetStaging(td.Width, td.Height, td.Format);
    m_ctx->CopyResource(staging.get(), m_duplicateMse.tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    bool same = true;
    for (UINT y = 0; y < td.Height && same; y++) {
        auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + (size_t)y * mapped.RowPitch);
        for (UINT x = 0; x < td.Width; x++) {
            if (row[x] > kDuplicateMse) {
                same = false;
                break;
            }
        }
    }
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
    return same;
}

void FrameGen::PushFrame(ID3D11Texture2D* source, UINT w, UINT h, double time, const FgOptions& options)
{
    EnsureFrames(w, h);

    // 빈 슬롯에 받아 보고, 앞 프레임과 같으면 버린다.
    Frame& incoming = m_frames[kFrames - 1];
    m_ctx->CopyResource(incoming.color.get(), source);
    BuildPyramid(incoming);
    if (m_count > 0 && IsDuplicate(incoming, m_frames[0])) {
        m_stats.duplicates++;
        return;
    }

    std::rotate(m_frames.begin(), m_frames.end() - 1, m_frames.end());
    Frame& f = m_frames[0];
    f.time = time;
    m_stats.realFrames++;
    m_count = std::min(m_count + 1, kFrames - 1);

    const double gap = m_count >= 2 ? time - m_frames[1].time : 0;
    m_tripleValid = m_pairValid && gap > 0 && gap < kPauseGap;
    m_pairValid = m_count >= 2 && gap > 0 && gap < kPauseGap;

    if (m_pairValid) {
        m_intervals.push_back(gap);
        if (m_intervals.size() > 9) m_intervals.pop_front();
        std::vector<double> sorted(m_intervals.begin(), m_intervals.end());
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        m_interval = sorted[sorted.size() / 2];
        m_stats.realFps = 1.0 / m_interval;
    }

    // 채점과 다음 추정이 flow를 덮어쓰기 전에 지금 쌍의 움직임을 옮겨 둔다. 이 쌍은 이제 한 쌍 앞이 된다.
    m_olderFlow = nullptr;
    if (m_tripleValid && m_liveFlow) {
        for (int i = 0; i < 2; i++) m_ctx->CopyResource(m_liveFlow->older[i].tex.get(), m_liveFlow->flow[i].tex.get());
        m_olderFlow = m_liveFlow;
    }

    TimerBegin(m_timerEstimate);
    if (m_pairValid) {
        UpdateMapTexture();
        FgParams params = Effective(m_profile ? m_profile->params : FgParams{}, options);
        FlowSet& set = GetFlowSet(params.cellPx);
        const bool useMap = options.useMap && params.useMap && m_profile && m_profile->mapSamples >= 20;

        if (options.learn && m_profile && m_tripleValid && time - m_lastEvalTime >= kEvalPeriod) {
            m_lastEvalTime = time;
            RunEvaluation(options);
        }

        // 채점이 같은 격자를 썼을 수 있으므로 실시간 움직임은 마지막에 계산한다.
        Estimate(m_frames[1], m_frames[0], params, set, true, useMap, m_interval);
        m_liveFlow = &set;

        if (options.learn && m_profile && time - m_lastSampleTime >= kSamplePeriod) {
            m_lastSampleTime = time;
            QueueFlowSample(set);
            if (time - m_lastGenreUpdate >= 5.0) {
                m_lastGenreUpdate = time;
                m_profile->UpdateGenre();
            }
        }
    } else {
        m_liveFlow = nullptr;
    }
    TimerEnd(m_timerEstimate);
    m_stats.estimateMs = m_timerEstimate.ms;

    // RIFE: 다음 표시가 시작되기 전에 결과가 오도록 프레임이 오는 대로 넘긴다.
    if (options.generator == FG_GENERATOR_RIFE && options.method == FG_METHOD_INTERPOLATE) {
        EnsureRife(options.quality, true);
        if (m_rife && m_rife->GetState() == RifeEngine::STATE_READY) {
            std::vector<float> current = RifePrepare(f);
            if (m_pairValid && m_rifePrevious.size() == current.size())
                m_rife->Submit(m_stats.realFrames, m_rifePrevious, current, (int)m_rifeW, (int)m_rifeH);
            m_rifePrevious = std::move(current);
        }
    }
}

void FrameGen::EnsureRife(int quality, bool adapt)
{
    if (!m_rife) {
        const std::wstring directory = RifeEngine::DefaultDirectory();
        m_rife = std::make_unique<RifeEngine>(directory);
    }

    // 프레임 간격 안에 끝나지 못하면 입력을 한 단계씩 줄인다.
    static const int kLadder[] = { 576, 384, 256, 192, 128 };
    if (adapt && m_rife->GetState() == RifeEngine::STATE_READY && m_rife->ResultCount() >= 8 && m_interval > 0 &&
        m_rife->AverageMs() > m_interval * 1000.0 * kRifeBudget) {
        for (int height : kLadder) {
            if (height < (int)m_rifeH) {
                m_rifeHeightLimit = height;
                break;
            }
        }
    }

    // 입력 크기: 품질에 따른 높이에 맞추고 64의 배수로 맞춘다. 원본보다 크게 하지는 않는다.
    static const int kHeights[FG_QUALITY_COUNT] = { 256, 384, 576 };
    int target = std::min(kHeights[std::clamp(quality, 0, FG_QUALITY_COUNT - 1)], m_rifeHeightLimit);
    int h = std::max(64, std::min(target, (int)m_h + 63) / 64 * 64);
    int w = std::max(64, (int)std::lround((double)m_w / m_h * h / 64.0) * 64);
    m_stats.rifeHeight = h;
    if ((UINT)w != m_rifeW || (UINT)h != m_rifeH) {
        m_rifeW = w;
        m_rifeH = h;
        m_rifeInput = MakeTarget(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        m_rifePrevious.clear();
        m_rife->SetInputSize(w, h);
    }
}

std::vector<float> FrameGen::RifePrepare(const Frame& f)
{
    Cb cb = BaseCb(nullptr);
    cb.lvlSize[0] = (int)m_rifeW;
    cb.lvlSize[1] = (int)m_rifeH;
    Run(m_csResize.get(), { f.colorSrv.get() }, m_rifeInput.uav.get(), cb, (m_rifeW + 7) / 8, (m_rifeH + 7) / 8);

    auto staging = GetStaging(m_rifeW, m_rifeH, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_ctx->CopyResource(staging.get(), m_rifeInput.tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));

    // RIFE는 BGR 순서로 학습됐다. 평면 3채널로 나눈다.
    const size_t pixels = (size_t)m_rifeW * m_rifeH;
    std::vector<float> out(pixels * 3);
    for (UINT y = 0; y < m_rifeH; y++) {
        auto row = static_cast<const uint8_t*>(mapped.pData) + (size_t)y * mapped.RowPitch;
        const size_t base = (size_t)y * m_rifeW;
        for (UINT x = 0; x < m_rifeW; x++) {
            out[base + x] = row[x * 4 + 2] / 255.0f;
            out[pixels + base + x] = row[x * 4 + 1] / 255.0f;
            out[pixels * 2 + base + x] = row[x * 4] / 255.0f;
        }
    }
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
    return out;
}

void FrameGen::RifeUpload(const RifeEngine::Result& result)
{
    std::swap(m_rifeResults[0], m_rifeResults[1]);
    RifeFlow& slot = m_rifeResults[0];
    if ((UINT)result.w != slot.w || (UINT)result.h != slot.h) {
        slot.w = result.w;
        slot.h = result.h;
        slot.flow = MakeTarget(result.w, result.h, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
        slot.mask = MakeTarget(result.w, result.h, DXGI_FORMAT_R32_FLOAT, false);
    }
    slot.seq = result.seq;
    m_ctx->UpdateSubresource(slot.flow.tex.get(), 0, nullptr, result.flow.data(), result.w * 4 * sizeof(float), 0);
    m_ctx->UpdateSubresource(slot.mask.tex.get(), 0, nullptr, result.mask.data(), result.w * sizeof(float), 0);
}

void FrameGen::RifeCompose(const Frame& a, const Frame& b, const RifeFlow& rife, float t, ID3D11UnorderedAccessView* out)
{
    Cb cb = BaseCb(nullptr);
    cb.t = t;
    cb.lvlSize[0] = (int)rife.w;
    cb.lvlSize[1] = (int)rife.h;
    Run(m_csRifeCompose.get(), { a.colorSrv.get(), b.colorSrv.get(), rife.flow.srv.get(), rife.mask.srv.get() }, out, cb,
        (m_w + 7) / 8, (m_h + 7) / 8);
}

const FrameGen::RifeFlow* FrameGen::FindRife(long long seq) const
{
    for (const RifeFlow& result : m_rifeResults)
        if (result.seq == seq) return &result;
    return nullptr;
}

ID3D11ShaderResourceView* FrameGen::Render(double now, const FgOptions& options)
{
    PollReadbacks();
    TimerPoll(m_timerEstimate);
    m_stats.estimateMs = m_timerEstimate.ms;

    const bool wantRife = options.generator == FG_GENERATOR_RIFE && options.method == FG_METHOD_INTERPOLATE;
    if (m_rife && wantRife) {
        RifeEngine::Result result;
        if (m_rife->TakeResult(result)) {
            // 보관 중인 프레임 쌍의 결과만 쓴다. 더 늦은 결과는 이미 지나갔다.
            if (result.seq >= m_stats.realFrames - 1) RifeUpload(result);
        }
        m_stats.rifeState = m_rife->GetState();
        m_stats.rifeMs = m_rife->AverageMs();
    } else {
        m_stats.rifeState = -1;
    }

    if (m_count == 0) return nullptr;
    const Frame& b = m_frames[0];
    m_held = b.colorSrv.get();
    m_stats.noRoom = false;
    if (!m_pairValid || !m_liveFlow) {
        m_stats.displayT = 1;
        return b.colorSrv.get();
    }
    const Frame& a = m_frames[1];
    m_held = a.colorSrv.get();

    // 진짜 프레임이 주사율만큼 나오면 넣을 자리가 없으므로 그대로 보여 준다 (지연도 늘리지 않는다).
    const double vsync = 1.0 / std::max(options.refreshHz, 1.0);
    if (m_interval < vsync * 1.15 && !options.visualize) {
        m_stats.noRoom = true;
        m_stats.displayT = 1;
        m_held = b.colorSrv.get();
        return b.colorSrv.get();
    }

    // 표시 시계: 캡처 시각의 흔들림이 화면에 그대로 나타나지 않도록 천천히 따라간다.
    const bool extrapolate = options.method == FG_METHOD_EXTRAPOLATE;
    double delay = extrapolate ? 0.0 : m_interval + kDisplayMargin;
    // RIFE 결과를 기다릴 시간만큼 화면을 더 늦춘다. 지연이 그만큼 늘어난다.
    // 결과가 프레임 간격 안에 나올 때만 늦춘다. 그보다 느리면 어차피 블록 매칭으로 보여 주므로 지연만 늘어난다.
    const bool rifeReady = wantRife && m_stats.rifeState == RifeEngine::STATE_READY && m_stats.rifeMs > 0 && m_stats.rifeMs < m_interval * 1000.0;
    if (rifeReady) delay += m_stats.rifeMs / 1000.0;
    const double target = now - delay;
    m_clock += now - m_lastRender;
    m_lastRender = now;
    double error = target - m_clock;
    if (std::abs(error) > m_interval) m_clock = target;
    else m_clock += error * 0.1;

    // 게임 프레임 간격이 고르지 않으면 짧은 간격의 쌍이 도착했을 때 표시 시계가 아직 앞 쌍에 있다.
    // 이때 지금 쌍만 쓰면 시점이 0에 묶여 멈췄다가 건너뛰므로, 시계가 있는 쌍을 찾아 보여 준다.
    // RIFE는 표시 시계가 결과가 이미 나온 쌍을 지나고 있으면 새 결과가 나올 때까지 앞 쌍을 계속 보여 준다.
    const RifeFlow* rifeNew = rifeReady ? FindRife(m_stats.realFrames) : nullptr;
    const RifeFlow* rifeOld = rifeReady && m_tripleValid ? FindRife(m_stats.realFrames - 1) : nullptr;
    const bool olderUsable = !extrapolate && m_tripleValid && m_count >= 3 && (m_olderFlow || rifeOld);
    const bool usePrevious = olderUsable && (m_clock < a.time || (rifeOld && !rifeNew));
    const Frame* showA = usePrevious ? &m_frames[2] : &a;
    const Frame* showB = usePrevious ? &m_frames[1] : &b;
    const RifeFlow* rife = usePrevious ? rifeOld : rifeNew;
    m_held = showA->colorSrv.get();

    float t = (float)((m_clock - showA->time) / (showB->time - showA->time));
    if (extrapolate) t = std::clamp(t, 1.0f, kMaxExtrapolate);
    else t = std::clamp(t, 0.0f, 1.0f);
    if (options.multiplier >= 2) t = std::floor(t * options.multiplier) / options.multiplier;
    m_stats.displayT = t;
    m_stats.contentTime = showA->time + (showB->time - showA->time) * t;

    if (options.visualize) {
        Project(*m_liveFlow, std::min(t, 1.0f));
        Cb cb = BaseCb(m_liveFlow);
        Run(m_csVisualize.get(), { nullptr, b.colorSrv.get(), m_liveFlow->motion.srv.get() }, m_output.uav.get(), cb, (m_w + 7) / 8,
            (m_h + 7) / 8);
        return m_output.srv.get();
    }

    if (!extrapolate && t <= 0.005f) return showA->colorSrv.get();
    if (t >= 0.995f && t <= 1.005f) return showB->colorSrv.get();

    TimerBegin(m_timerSynth);
    if (rife) {
        RifeCompose(*showA, *showB, *rife, t, m_output.uav.get());
        m_stats.rifeShown++;
    } else {
        // RIFE 결과가 없으면 블록 매칭으로 대신한다.
        if (wantRife) m_stats.rifeMissed++;
        const FgParams params = Effective(m_profile ? m_profile->params : FgParams{}, options);
        if (usePrevious) {
            FlowSet olderSet = *m_olderFlow;
            olderSet.flow = olderSet.older;
            Synthesize(*showA, *showB, olderSet, t, params, m_output.uav.get());
        } else {
            Synthesize(a, b, *m_liveFlow, t, params, m_output.uav.get());
        }
    }
    TimerEnd(m_timerSynth);
    m_stats.synthMs = m_timerSynth.ms;
    return m_output.srv.get();
}

ID3D11ShaderResourceView* FrameGen::HeldFrame() const
{
    return m_held;
}

// ---------------------------------------------------------------------------
// 슬로모션 테스트
// 실시간 제약 없이 표시 프레임을 전부 만들어 두므로, 느린 GPU에서도 목표 fps의 화면을 빠짐없이 볼 수 있다.
// 움직임 추정과 합성은 실시간 경로와 같은 셰이더를 쓴다. 표시 지연과 프레임 고르기는 이 방법으로 확인할 수 없다.
// ---------------------------------------------------------------------------

void FrameGen::SlowMoBegin(ID3D11Texture2D* source, UINT w, UINT h, int capacity)
{
    SlowMoClear();
    Reset();
    EnsureFrames(w, h);
    m_slowMoReal.reserve(capacity);
    for (int i = 0; i < capacity; i++) {
        m_slowMoReal.push_back(MakeTarget(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false));
        m_ctx->CopyResource(m_slowMoReal.back().tex.get(), source);
    }
    m_slowMoTimes.assign(capacity, 0.0);

    // 처음 쓰는 텍스처에 드라이버가 메모리를 잡느라 멈추는 일이 녹화 중에 생기지 않도록 여기서 끝날 때까지 기다린다.
    auto staging = GetStaging(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    m_ctx->CopyResource(staging.get(), m_slowMoReal.back().tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
    m_rifeHeightLimit = 10000;  // 시간 제한이 없으므로 실시간에서 낮춘 입력 높이를 되돌린다
}

bool FrameGen::SlowMoRecord(ID3D11Texture2D* source, UINT w, UINT h, double time)
{
    if (w != m_w || h != m_h) return false;
    if (SlowMoFull()) return true;

    Frame& incoming = m_frames[kFrames - 1];
    m_ctx->CopyResource(incoming.color.get(), source);
    BuildPyramid(incoming);
    if (m_count > 0 && IsDuplicate(incoming, m_frames[0])) {
        m_stats.duplicates++;
        return true;
    }
    std::rotate(m_frames.begin(), m_frames.end() - 1, m_frames.end());
    m_count = std::min(m_count + 1, kFrames - 1);

    if (m_slowMoFirstSeen < 0) m_slowMoFirstSeen = time;
    if (time - m_slowMoFirstSeen < kSlowMoWarmup) return true;

    m_ctx->CopyResource(m_slowMoReal[m_slowMoRecorded].tex.get(), source);
    m_slowMoTimes[m_slowMoRecorded] = time;
    m_slowMoRecorded++;
    return true;
}

double FrameGen::SlowMoSpan() const
{
    return m_slowMoRecorded >= 2 ? m_slowMoTimes[m_slowMoRecorded - 1] - m_slowMoTimes[0] : 0;
}

bool FrameGen::SlowMoStartGenerating(int targetFps, int maxFrames)
{
    if (m_slowMoRecorded < 2) return false;
    m_slowMoReal.resize(m_slowMoRecorded);
    m_slowMoCounts.assign(m_slowMoRecorded - 1, 0);

    // 반올림 오차가 쌓이지 않도록 첫 프레임부터의 시간으로 정한다.
    int emitted = 0;
    for (int i = 0; i + 1 < m_slowMoRecorded; i++) {
        const int end = std::min(maxFrames, (int)std::lround((m_slowMoTimes[i + 1] - m_slowMoTimes[0]) * targetFps));
        m_slowMoCounts[i] = std::max(0, end - emitted);
        emitted = std::max(emitted, end);
    }
    m_slowMoTotal = emitted;
    m_slowMoNext = 0;
    m_slowMoFrames.reserve(emitted);
    return emitted > 0;
}

bool FrameGen::SlowMoGenerateStep(const FgOptions& options)
{
    if (m_slowMoNext >= (int)m_slowMoCounts.size()) return true;

    bool useRife = options.generator == FG_GENERATOR_RIFE;
    m_slowMoRifeLoading = false;
    if (useRife) {
        EnsureRife(options.quality, false);
        const RifeEngine::State state = m_rife->GetState();
        if (state == RifeEngine::STATE_LOADING) {
            m_slowMoRifeLoading = true;
            return false;
        }
        useRife = state == RifeEngine::STATE_READY;
    }

    const int index = m_slowMoNext++;
    const int count = m_slowMoCounts[index];
    if (count == 0) return m_slowMoNext >= (int)m_slowMoCounts.size();

    const Target& first = m_slowMoReal[index];
    const Target& second = m_slowMoReal[index + 1];
    const double gap = m_slowMoTimes[index + 1] - m_slowMoTimes[index];

    SlowMoFrame shown;
    shown.srv = shown.held = first.srv.get();
    shown.real = index;
    m_slowMoFrames.push_back(shown);

    if (count > 1 && gap >= kPauseGap) {
        // 게임이 멈췄던 구간은 보간하지 않고 진짜 프레임을 그대로 둔다.
        for (int j = 1; j < count; j++) m_slowMoFrames.push_back(shown);
    } else if (count > 1) {
        Frame& a = m_frames[1];
        Frame& b = m_frames[0];
        m_ctx->CopyResource(a.color.get(), first.tex.get());
        m_ctx->CopyResource(b.color.get(), second.tex.get());
        BuildPyramid(a);
        BuildPyramid(b);

        if (useRife) {
            RifeEngine::Result result;
            useRife = m_rife->RunBlocking(RifePrepare(a), RifePrepare(b), (int)m_rifeW, (int)m_rifeH, result);
            if (useRife) {
                RifeUpload(result);
                m_slowMoRifeHeight = (int)m_rifeH;
            }
        }
        const FgParams params = Effective(m_profile ? m_profile->params : FgParams{}, options);
        FlowSet& set = GetFlowSet(params.cellPx);
        if (!useRife) {
            UpdateMapTexture();
            const bool useMap = options.useMap && params.useMap && m_profile && m_profile->mapSamples >= 20;
            Estimate(a, b, params, set, true, useMap, gap);
        }
        for (int j = 1; j < count; j++) {
            m_slowMoGenerated.push_back(MakeTarget(m_w, m_h, DXGI_FORMAT_R8G8B8A8_UNORM, true));
            const Target& out = m_slowMoGenerated.back();
            shown.srv = out.srv.get();
            shown.t = (float)j / count;
            shown.generated = true;
            if (useRife) RifeCompose(a, b, m_rifeResults[0], shown.t, out.uav.get());
            else Synthesize(a, b, set, shown.t, params, out.uav.get());
            m_slowMoFrames.push_back(shown);
        }
    }
    return m_slowMoNext >= (int)m_slowMoCounts.size();
}

std::vector<uint32_t> FrameGen::SlowMoReadFrame(int index)
{
    const SlowMoFrame& frame = m_slowMoFrames[index];
    com_ptr<ID3D11Resource> resource;
    frame.srv->GetResource(resource.put());
    auto tex = resource.as<ID3D11Texture2D>();
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    auto staging = GetStaging(td.Width, td.Height, td.Format);
    m_ctx->CopyResource(staging.get(), tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<uint32_t> out((size_t)td.Width * td.Height);
    for (UINT y = 0; y < td.Height; y++)
        memcpy(&out[(size_t)y * td.Width], static_cast<const char*>(mapped.pData) + (size_t)y * mapped.RowPitch, td.Width * 4);
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
    if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
        for (uint32_t& c : out) c = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    return out;
}

void FrameGen::SlowMoClear()
{
    m_slowMoFrames.clear();
    m_slowMoGenerated.clear();
    m_slowMoReal.clear();
    m_slowMoTimes.clear();
    m_slowMoCounts.clear();
    m_slowMoRecorded = m_slowMoNext = m_slowMoTotal = m_slowMoRifeHeight = 0;
    m_slowMoFirstSeen = -1;
    m_slowMoRifeLoading = false;
}

// ---------------------------------------------------------------------------
// 자체 테스트용
// ---------------------------------------------------------------------------

void FrameGen::TestLoadFrame(int slot, const uint32_t* rgba, UINT w, UINT h)
{
    EnsureFrames(w, h);
    Frame& f = m_frames[slot];
    // 프레임 텍스처는 BGRA다.
    std::vector<uint32_t> bgra((size_t)w * h);
    for (size_t i = 0; i < bgra.size(); i++) {
        uint32_t c = rgba[i];
        bgra[i] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    }
    m_ctx->UpdateSubresource(f.color.get(), 0, nullptr, bgra.data(), w * 4, 0);
    BuildPyramid(f);
}

float FrameGen::BlockingMsePsnr(ID3D11ShaderResourceView* x, ID3D11ShaderResourceView* y)
{
    D3D11_TEXTURE2D_DESC td;
    m_mse.tex->GetDesc(&td);
    Cb cb = BaseCb(nullptr);
    cb.gridSize[0] = (int)td.Width;
    cb.gridSize[1] = (int)td.Height;
    Run(m_csMse.get(), { x, y }, m_mse.uav.get(), cb, (td.Width + 3) / 4, (td.Height + 3) / 4);
    auto staging = GetStaging(td.Width, td.Height, td.Format);
    m_ctx->CopyResource(staging.get(), m_mse.tex.get());
    float mse = MseFromStaging(staging.get(), td.Width, td.Height);
    RecycleStaging(staging);
    return PsnrFromMse(mse);
}

float FrameGen::TestBlendPsnr(int a, int b, int reference, float t)
{
    Blend(m_frames[a], m_frames[b], t, m_evalOutput.uav.get());
    return BlockingMsePsnr(m_evalOutput.srv.get(), m_frames[reference].colorSrv.get());
}

float FrameGen::TestHoldPsnr(int a, int reference)
{
    return BlockingMsePsnr(m_frames[a].colorSrv.get(), m_frames[reference].colorSrv.get());
}

FrameGen::TestResult FrameGen::TestSynthPsnr(int a, int b, int reference, float t, const FgParams& params, bool useMap, double interval)
{
    TestResult result;
    FlowSet& set = GetFlowSet(params.cellPx);

    TimerBegin(m_timerEstimate);
    Estimate(m_frames[a], m_frames[b], params, set, false, useMap || params.useMap, interval);
    TimerEnd(m_timerEstimate);

    TimerBegin(m_timerSynth);
    Synthesize(m_frames[a], m_frames[b], set, t, params, m_evalOutput.uav.get());
    TimerEnd(m_timerSynth);

    result.psnr = BlockingMsePsnr(m_evalOutput.srv.get(), m_frames[reference].colorSrv.get());
    m_ctx->Flush();
    for (int tries = 0; tries < 50; tries++) {
        TimerPoll(m_timerEstimate);
        TimerPoll(m_timerSynth);
        bool waiting = false;
        for (auto& s : m_timerEstimate.slots) waiting |= s.pending;
        for (auto& s : m_timerSynth.slots) waiting |= s.pending;
        if (!waiting) break;
        Sleep(1);
    }
    result.estimateMs = (float)m_timerEstimate.ms;
    result.synthMs = (float)m_timerSynth.ms;
    m_liveFlow = &set;
    return result;
}

std::vector<uint32_t> FrameGen::TestReadOutput()
{
    D3D11_TEXTURE2D_DESC td;
    m_evalOutput.tex->GetDesc(&td);
    auto staging = GetStaging(td.Width, td.Height, td.Format);
    m_ctx->CopyResource(staging.get(), m_evalOutput.tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<uint32_t> out((size_t)td.Width * td.Height);
    for (UINT y = 0; y < td.Height; y++)
        memcpy(&out[(size_t)y * td.Width], static_cast<const char*>(mapped.pData) + (size_t)y * mapped.RowPitch, td.Width * 4);
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
    return out;
}

std::vector<uint32_t> FrameGen::TestReadVisualize(int a, int b, const FgParams& params, float t)
{
    FlowSet& set = GetFlowSet(params.cellPx);
    Project(set, t);
    Cb cb = BaseCb(&set);
    Run(m_csVisualize.get(), { nullptr, m_frames[b].colorSrv.get(), set.motion.srv.get() }, m_evalOutput.uav.get(), cb, (m_w + 7) / 8,
        (m_h + 7) / 8);
    (void)a;
    return TestReadOutput();
}

void FrameGen::TestAddFlowSample(const FgParams& params, double interval)
{
    if (!m_profile) return;
    FlowSet& set = GetFlowSet(params.cellPx);
    auto staging = GetStaging(set.gridW, set.gridH, DXGI_FORMAT_R32G32B32A32_FLOAT);
    m_ctx->CopyResource(staging.get(), set.flow[0].tex.get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    m_profile->AddFlowSample(static_cast<const float*>(mapped.pData), (int)set.gridW, (int)set.gridH,
                             (int)(mapped.RowPitch / sizeof(float)), interval);
    m_ctx->Unmap(staging.get(), 0);
    RecycleStaging(staging);
}

void FrameGen::TestUpdateMapTexture()
{
    UpdateMapTexture();
}

bool FrameGen::TestRifeRun(int a, int b, int quality)
{
    EnsureRife(quality, false);
    while (m_rife->GetState() == RifeEngine::STATE_LOADING) Sleep(10);
    if (m_rife->GetState() != RifeEngine::STATE_READY) return false;

    RifeEngine::Result result;
    if (!m_rife->RunBlocking(RifePrepare(m_frames[a]), RifePrepare(m_frames[b]), (int)m_rifeW, (int)m_rifeH, result)) return false;
    RifeUpload(result);
    m_rifeLastInferMs = (float)result.ms;
    return true;
}

FrameGen::RifeTestResult FrameGen::TestRifePsnr(int a, int b, int reference, float t)
{
    RifeTestResult r;
    RifeCompose(m_frames[a], m_frames[b], m_rifeResults[0], t, m_evalOutput.uav.get());
    r.psnr = BlockingMsePsnr(m_evalOutput.srv.get(), m_frames[reference].colorSrv.get());
    r.inferMs = m_rifeLastInferMs;
    return r;
}

std::string FrameGen::TestRifeError()
{
    return m_rife ? m_rife->GetError() : std::string();
}
