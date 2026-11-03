#pragma once

// 프레임 생성: 게임의 진짜 프레임 사이에 움직임을 보정한 중간 프레임을 만든다.
//
// 흐름: PushFrame(진짜 프레임이 올 때) -> 휘도 피라미드 -> 양방향 블록 매칭 -> 양방향 일치 검사
//       Render(화면을 그릴 때마다)     -> 시점 t 계산 -> 투영 -> 빈칸 채우기 -> 합성
//
// 설정 자동 조정: 연속된 진짜 프레임 F0, F1, F2 중 F1을 가리고 F0과 F2로 만든 뒤 실제 F1과 비교한다.

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "game_profile.h"
#include "rife.h"

using winrt::com_ptr;

enum FgQuality { FG_QUALITY_FAST, FG_QUALITY_NORMAL, FG_QUALITY_HIGH, FG_QUALITY_COUNT };
enum FgMethod { FG_METHOD_INTERPOLATE, FG_METHOD_EXTRAPOLATE, FG_METHOD_COUNT };
enum FgGenerator { FG_GENERATOR_BLOCK, FG_GENERATOR_RIFE, FG_GENERATOR_COUNT };

struct FgOptions {
    int multiplier = 0;      // 0: 주사율까지 자동으로 채우기, 2~4: 진짜 프레임 사이를 그 수만큼 나눈다
    int quality = FG_QUALITY_NORMAL;
    int method = FG_METHOD_INTERPOLATE;
    int generator = FG_GENERATOR_BLOCK;
    bool visualize = false;  // 움직임을 색으로 표시한다
    bool learn = true;       // 게임별 학습
    bool useMap = true;      // 게임별 화면 지도의 움직임을 후보로 쓴다
    double refreshHz = 60;
};

struct FgStats {
    double realFps = 0;     // 프레임 간격의 중앙값으로 구한 진짜 프레임 수
    double estimateMs = 0;  // 움직임 추정 GPU 시간 (진짜 프레임마다)
    double synthMs = 0;     // 중간 프레임 합성 GPU 시간 (화면을 그릴 때마다)
    double displayT = 0;    // 마지막으로 그린 시점
    double contentTime = 0; // 마지막으로 그린 화면이 게임 시간으로 언제인지 (초, 진행이 고른지 확인용)
    bool noRoom = false;    // 진짜 프레임이 주사율만큼 나와 넣을 자리가 없다
    long long duplicates = 0;  // 내용이 같아 버린 캡처 프레임 수
    long long realFrames = 0;  // 받아들인 진짜 프레임 수

    // RIFE
    int rifeState = -1;        // -1: 쓰지 않음, 나머지는 RifeEngine::State
    int rifeHeight = 0;        // 지금 신경망 입력 높이
    double rifeMs = 0;         // 신경망 한 번에 걸린 시간
    long long rifeShown = 0;   // RIFE로 합성해 보여 준 횟수
    long long rifeMissed = 0;  // 결과가 늦어 블록 매칭으로 대신한 횟수
};

class FrameGen {
public:
    FrameGen(ID3D11Device* device, ID3D11DeviceContext* context);

    void SetProfile(GameProfile* profile) { m_profile = profile; }
    void Reset();

    // 새 진짜 프레임. time은 캡처 시각(초)
    void PushFrame(ID3D11Texture2D* source, UINT w, UINT h, double time, const FgOptions& options);
    // 지금 화면에 보여 줄 영상. 아직 프레임이 없으면 nullptr
    ID3D11ShaderResourceView* Render(double now, const FgOptions& options);
    // 비교 모드 왼쪽에 쓰는 보간 없는 영상 (보간 쪽과 시점을 맞춘다)
    ID3D11ShaderResourceView* HeldFrame() const;

    const FgStats& Stats() const { return m_stats; }

    // 슬로모션 테스트: 짧게 녹화한 진짜 프레임 사이를 미리 채워 두고 한 장씩 보여 준다 ----------
    struct SlowMoFrame {
        ID3D11ShaderResourceView* srv = nullptr;   // 보여 줄 영상
        ID3D11ShaderResourceView* held = nullptr;  // 보간이 없으면 이 시점에 보일 진짜 프레임 (비교 모드 왼쪽)
        int real = 0;      // 몇 번째 진짜 프레임 뒤인지
        float t = 0;       // 다음 진짜 프레임까지의 시점
        bool generated = false;
    };
    void SlowMoBegin(ID3D11Texture2D* source, UINT w, UINT h, int capacity);
    // 진짜 프레임을 넣는다. 크기가 바뀌었으면 false. 내용이 같은 프레임은 버린다.
    bool SlowMoRecord(ID3D11Texture2D* source, UINT w, UINT h, double time);
    double SlowMoSpan() const;  // 녹화한 첫 프레임과 마지막 프레임의 시간 차 (초)
    bool SlowMoFull() const { return m_slowMoRecorded >= (int)m_slowMoReal.size(); }
    // 목표 fps에 맞춰 프레임 쌍마다 몇 장씩 나눌지 정한다 (최대 maxFrames장). 진짜 프레임이 모자라면 false.
    bool SlowMoStartGenerating(int targetFps, int maxFrames);
    // 프레임 쌍 하나를 처리한다. 모두 끝나면 true (화면이 멈추지 않도록 그릴 때마다 조금씩 부른다)
    bool SlowMoGenerateStep(const FgOptions& options);
    const std::vector<SlowMoFrame>& SlowMoFrames() const { return m_slowMoFrames; }
    int SlowMoTotal() const { return m_slowMoTotal; }
    int SlowMoRealCount() const { return m_slowMoRecorded; }
    int SlowMoRifeHeight() const { return m_slowMoRifeHeight; }  // RIFE로 만들었으면 입력 높이, 아니면 0
    bool SlowMoRifeLoading() const { return m_slowMoRifeLoading; }
    std::vector<uint32_t> SlowMoReadFrame(int index);  // RGBA로 읽어 온다 (테스트용)
    void SlowMoClear();

    // 자체 테스트용: 슬롯에 영상을 넣고 합성 결과를 정답과 비교한다 -----------------
    struct TestResult {
        float psnr = 0;
        float estimateMs = 0, synthMs = 0;
    };
    void TestLoadFrame(int slot, const uint32_t* rgba, UINT w, UINT h);
    float TestBlendPsnr(int a, int b, int reference, float t);
    float TestHoldPsnr(int a, int reference);
    TestResult TestSynthPsnr(int a, int b, int reference, float t, const FgParams& params, bool useMap, double interval);
    // 합성 결과를 읽어 온다 (RGBA)
    std::vector<uint32_t> TestReadOutput();
    std::vector<uint32_t> TestReadVisualize(int a, int b, const FgParams& params, float t);
    // 지금 움직임 결과를 화면 지도에 넣는다
    void TestAddFlowSample(const FgParams& params, double interval);
    void TestUpdateMapTexture();
    void TestResetTimers() { m_timerEstimate.ms = m_timerSynth.ms = 0; }
    // RIFE를 쓸 수 있으면 슬롯 a, b로 움직임을 구해 합성하고 정답과 비교한다. 쓸 수 없으면 psnr이 음수다.
    struct RifeTestResult {
        float psnr = -1;
        float inferMs = 0, totalMs = 0;
    };
    bool TestRifeRun(int a, int b, int quality);  // 신경망 실행. 결과는 다음 호출까지 유지된다
    RifeTestResult TestRifePsnr(int a, int b, int reference, float t);
    std::string TestRifeError();

private:
    static constexpr int kLevels = 5;  // 원본, 1/2, 1/4, 1/8, 1/16
    static constexpr int kFrames = 4;  // 최근 3장 + 다음 프레임을 받을 빈 슬롯
    static constexpr int kDuplicateLevel = 3;  // 중복 프레임 검사에 쓰는 단계 (1/8)

    struct Frame {
        com_ptr<ID3D11Texture2D> color;
        com_ptr<ID3D11ShaderResourceView> colorSrv;
        std::array<com_ptr<ID3D11ShaderResourceView>, kLevels> lumaSrv;
        std::array<com_ptr<ID3D11UnorderedAccessView>, kLevels> lumaUav;
        double time = 0;
    };

    struct Target {
        com_ptr<ID3D11Texture2D> tex;
        com_ptr<ID3D11ShaderResourceView> srv;
        com_ptr<ID3D11UnorderedAccessView> uav;
    };

    // 칸 크기별 움직임 격자
    struct FlowSet {
        int cellPx = 0;
        UINT gridW = 0, gridH = 0;
        std::array<Target, kLevels> pass;  // 단계별 중간 결과 (마지막 단계는 raw에 쓴다)
        std::array<Target, 2> raw;         // 일치 검사 전 (0: A->B, 1: B->A)
        std::array<Target, 2> flow;        // 일치 검사 후
        std::array<Target, 2> older;       // 한 쌍 앞 프레임 쌍의 flow
        Target splat;
        Target motion;
        bool hasHistory = false;
    };

    // 프레임 쌍 하나의 RIFE 결과
    struct RifeFlow {
        Target flow, mask;
        UINT w = 0, h = 0;
        long long seq = -1;
    };

    struct Pending {
        com_ptr<ID3D11Texture2D> staging;
        int kind = 0;      // 0: 채점, 1: 움직임 표본
        int evalGroup = 0;
        int evalIndex = 0;  // 0: 현재 값, 1: 후보, 2: 블렌딩
        UINT w = 0, h = 0;
        double interval = 0;
    };

    struct GpuTimer {
        struct Slot {
            com_ptr<ID3D11Query> disjoint, begin, end;
            bool pending = false;
        };
        std::array<Slot, 6> slots;
        int next = 0;
        int active = -1;
        double ms = 0;
    };

    struct Cb;

    com_ptr<ID3D11ComputeShader> CreateCs(const char* entry);
    void EnsureFrames(UINT w, UINT h);
    FlowSet& GetFlowSet(int cellPx);
    Target MakeTarget(UINT w, UINT h, DXGI_FORMAT format, bool uav);
    void Run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
             const Cb& cb, UINT groupsX, UINT groupsY);
    Cb BaseCb(const FlowSet* set) const;

    void BuildPyramid(Frame& f);
    void Estimate(const Frame& a, const Frame& b, const FgParams& params, FlowSet& set, bool temporal, bool useMap, double interval);
    void Project(const FlowSet& set, float t);
    void Synthesize(const Frame& a, const Frame& b, const FlowSet& set, float t, const FgParams& params, ID3D11UnorderedAccessView* out);
    void Blend(const Frame& a, const Frame& b, float t, ID3D11UnorderedAccessView* out);
    void QueueMse(ID3D11ShaderResourceView* x, ID3D11ShaderResourceView* y, int group, int index);
    float MseFromStaging(ID3D11Texture2D* staging, UINT w, UINT h);
    float BlockingMsePsnr(ID3D11ShaderResourceView* x, ID3D11ShaderResourceView* y);
    bool IsDuplicate(const Frame& a, const Frame& b);

    void RunEvaluation(const FgOptions& options);
    void QueueFlowSample(const FlowSet& set);
    void PollReadbacks();
    void UpdateMapTexture();

    void EnsureRife(int quality, bool adapt);
    std::vector<float> RifePrepare(const Frame& f);
    void RifeUpload(const RifeEngine::Result& result);
    void RifeCompose(const Frame& a, const Frame& b, const RifeFlow& rife, float t, ID3D11UnorderedAccessView* out);
    const RifeFlow* FindRife(long long seq) const;
    FgParams Effective(const FgParams& params, const FgOptions& options) const;
    com_ptr<ID3D11Texture2D> GetStaging(UINT w, UINT h, DXGI_FORMAT format);
    void RecycleStaging(com_ptr<ID3D11Texture2D> tex);

    void TimerBegin(GpuTimer& timer);
    void TimerEnd(GpuTimer& timer);
    void TimerPoll(GpuTimer& timer);

    com_ptr<ID3D11Device> m_device;
    com_ptr<ID3D11DeviceContext> m_ctx;
    com_ptr<ID3D11ComputeShader> m_csLuma, m_csDown, m_csMatch, m_csConsist, m_csProject, m_csFill, m_csCompose, m_csBlend,
        m_csVisualize, m_csMse, m_csResize, m_csRifeCompose;
    com_ptr<ID3D11Buffer> m_cb;
    com_ptr<ID3D11SamplerState> m_sampler;

    UINT m_w = 0, m_h = 0;
    std::array<UINT, kLevels> m_lvlW{}, m_lvlH{};
    std::array<Frame, kFrames> m_frames;  // 0이 최신
    int m_count = 0;
    bool m_pairValid = false;
    bool m_tripleValid = false;

    std::vector<FlowSet> m_flowSets;
    FlowSet* m_liveFlow = nullptr;
    FlowSet* m_olderFlow = nullptr;  // 한 쌍 앞 프레임 쌍의 움직임이 들어 있는 격자 (없으면 nullptr)
    Target m_output;
    Target m_evalOutput;
    Target m_mse;
    Target m_duplicateMse;
    com_ptr<ID3D11Texture2D> m_mapTex;
    com_ptr<ID3D11ShaderResourceView> m_mapSrv;
    int m_mapUploaded = -1;
    ID3D11ShaderResourceView* m_held = nullptr;

    std::deque<double> m_intervals;
    double m_interval = 0;
    double m_clock = 0, m_lastRender = 0;
    double m_lastEvalTime = 0, m_lastSampleTime = 0;
    double m_lastGenreUpdate = 0;

    GameProfile* m_profile = nullptr;
    std::deque<Pending> m_pending;
    std::vector<com_ptr<ID3D11Texture2D>> m_stagingPool;
    int m_evalGroup = 0;
    std::array<float, 3> m_evalScores{};
    int m_evalFilled = 0;
    int m_evalCurrentGroup = -1;

    std::unique_ptr<RifeEngine> m_rife;
    UINT m_rifeW = 0, m_rifeH = 0;
    Target m_rifeInput;
    std::array<RifeFlow, 2> m_rifeResults;  // 0이 최신. 표시 시계가 앞 쌍에 있을 때를 위해 두 쌍을 보관한다
    std::vector<float> m_rifePrevious;  // 앞 프레임의 신경망 입력 (다시 줄이지 않도록 보관)
    int m_rifeHeightLimit = 10000;      // 너무 느려서 낮춘 입력 높이. 이번 실행 동안 유지한다
    float m_rifeLastInferMs = 0;

    std::vector<Target> m_slowMoReal;       // 녹화한 진짜 프레임 (미리 잡아 둔다)
    std::vector<double> m_slowMoTimes;
    std::vector<int> m_slowMoCounts;        // 프레임 쌍마다 나눌 수
    std::vector<Target> m_slowMoGenerated;  // 만든 프레임의 텍스처
    std::vector<SlowMoFrame> m_slowMoFrames;
    int m_slowMoRecorded = 0;
    double m_slowMoFirstSeen = -1;          // 녹화를 시작하고 처음 받은 프레임의 시각
    int m_slowMoNext = 0;
    int m_slowMoTotal = 0;
    int m_slowMoRifeHeight = 0;
    bool m_slowMoRifeLoading = false;

    GpuTimer m_timerEstimate, m_timerSynth;
    FgStats m_stats;
};
