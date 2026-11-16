#pragma once

// 게임별 프로필: 프레임 생성 설정 자동 조정(학습 1단계)과 화면 지도(학습 2단계)
// 실행 파일 옆의 profiles/게임이름.json에 저장한다. 게임 화면 자체는 저장하지 않는다.

#include <string>
#include <vector>

// 프레임 생성에서 게임마다 달라지는 값
struct FgParams {
    int cellPx = 4;           // 움직임 격자 한 칸의 크기 (원본 픽셀). 4면 1/4 해상도
    int coarseRadius = 12;    // 가장 굵은 단계의 탐색 범위 (1/16 영상의 픽셀)
    float smoothness = 0.0015f;  // 주변과 다른 벡터에 주는 벌점
    float blendTol = 0.08f;   // 합성할 때 허용하는 밝기 차이
    bool useMap = false;      // 화면 지도의 움직임을 후보로 쓴다. 채점에서 더 좋을 때만 켜진다

    bool operator==(const FgParams&) const = default;
};

enum FgGenre { GENRE_GENERAL, GENRE_RHYTHM, GENRE_SCROLL, GENRE_COUNT };
extern const char* const kGenreKeys[GENRE_COUNT];

// 화면을 kMapW x kMapH 칸으로 나눈 지도
constexpr int kMapW = 32;
constexpr int kMapH = 18;

struct MapCell {
    float vx = 0, vy = 0;   // 이 칸의 주된 움직임 (원본 크기 비율로 환산한 픽셀/초)
    float activity = 0;     // 움직이는 때의 비율
    float stillness = 0;    // 화면이 움직이는 동안에도 멈춰 있는 비율 (고정 UI)
    float coherence = 0;    // 움직임이 한 방향으로 모여 있는 정도
};

// 설정 자동 조정(학습 1단계) 하나. 보간과 외삽은 잘 맞는 값이 달라 따로 배운다.
struct FgLearner {
    FgParams params;
    int evalCount = 0;             // 게임 프레임으로 채점한 횟수
    int adoptedCount = 0;          // 더 좋은 값으로 바꾼 횟수
    float bestPsnr = 0;            // 지금 설정의 최근 점수
    std::vector<float> history;    // 지금 설정의 점수 (dB)
    std::vector<float> baseline;   // 같은 구간의 기준 점수 (dB). 보간은 단순 블렌딩, 외삽은 앞 프레임 그대로

    // 지금 시험 중인 후보 값. 같은 게임 프레임으로 현재 값과 짝지어 채점한다.
    FgParams Candidate() const { return m_candidate; }
    // 채점 결과 한 쌍(dB)을 넣는다. 후보가 충분히 좋으면 채택한다. mapSamples: 화면 지도 표본 수
    void AddEvaluation(float currentDb, float candidateDb, float baselineDb, int mapSamples);
    // 처음이나 불러온 뒤에 첫 후보를 정한다.
    void Start(int mapSamples);

private:
    void NextCandidate(int mapSamples);

    FgParams m_candidate;
    int m_axis = 0;       // 지금 바꿔 보는 값 (칸 크기, 탐색 범위, 벌점, 허용치, 화면 지도)
    int m_step = 0;
    int m_pairs = 0;
    int m_trials = 0;
    std::vector<float> m_deltas;  // 이번 후보의 채점마다 (후보 - 현재) dB
    float m_sumCurrent = 0, m_sumBaseline = 0;
    int m_historyCount = 0;
};

struct GameProfile {
    std::wstring name;
    int genre = GENRE_GENERAL;     // 통계로 판단한 유형

    // 보간: 가운데 게임 프레임을 가리고 양옆으로 다시 만들어 채점한다 (블록 매칭 보간, RIFE를 쓰는 동안의 채점).
    // 외삽: 앞의 두 게임 프레임으로 다음 프레임을 예측해 채점한다 (블록 매칭 외삽).
    FgLearner interp;
    FgLearner extrap;
    FgLearner& Learner(bool extrapolate) { return extrapolate ? extrap : interp; }
    const FgLearner& Learner(bool extrapolate) const { return extrapolate ? extrap : interp; }

    std::vector<MapCell> map;      // kMapW * kMapH
    int mapSamples = 0;

    GameProfile();

    // 화면 지도 --------------------------------------------------------------
    // 움직임 격자(xyzw float)를 칸별로 모아 지도를 갱신한다. interval: 진짜 프레임 간격(초)
    void AddFlowSample(const float* flow, int gridW, int gridH, int rowPitchFloats, double interval);
    void UpdateGenre();

    bool Save(const std::wstring& dir) const;
    bool Load(const std::wstring& path);

};

std::wstring ProfilesDirectory();
std::vector<std::wstring> ListProfiles(const std::wstring& dir);
std::wstring ProfilePath(const std::wstring& dir, const std::wstring& name);
