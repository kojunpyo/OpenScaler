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
    // 최근 채점에서 기준(보간은 단순 블렌딩, 외삽은 앞 프레임 그대로)보다 평균 몇 dB 좋은지.
    // 공유할 데이터가 더 좋아졌는지 비교하는 값이다. evals에 평균 낸 채점 수를 담는다 (0이면 아직 없음).
    float RecentGain(int& evals) const;

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

// 고정 UI 상자: 확대하는 동안 둘레의 화면이 움직여도 같은 자리에 그대로 있던 곳 (게임 화면 크기에 대한 비율 0~1).
// 픽셀이나 화면은 남기지 않고 위치, 크기, 확신도만 남긴다. 다른 사람의 데이터를 받거나 해상도, UI 배율이 바뀌어도
// 다음 확대에서 다시 확인해 맞는 상자는 확신도가 오르고, 확인되지 않는 상자는 줄다가 지워진다.
struct StillBox {
    float x = 0, y = 0, w = 0, h = 0;
    float confidence = 0;  // 확인된 확대 횟수 (확인되지 않은 확대마다 1씩 줄고 0이 되면 지운다)
    float aspect = 0;      // 마지막으로 확인했을 때의 게임 화면 가로/세로 비율 (다르면 시작할 때 쓰지 않는다)
};
constexpr int kStillBoxMax = 16;
constexpr float kStillBoxMaxConfidence = 8.0f;
constexpr float kStillBoxCover = 0.5f;  // 상자 넓이의 이만큼이 다른 쪽 상자들로 덮이면 같은 자리로 본다 (붙거나 나뉜 상자)
constexpr double kStillBoxMinMoving = 20.0;  // 화면이 이만큼(초) 움직인 확대에서만 상자를 확인하거나 줄인다

// 공유를 권하는 최소 채점 수 (RecentGain이 이만큼 모였을 때)
constexpr int kShareMinEvals = 200;

struct GameProfile {
    std::wstring name;             // 게임 이름 (실행 파일 이름). 공유한 파일을 가져올 때 이 이름으로 넣는다
    int genre = GENRE_GENERAL;     // 통계로 판단한 유형

    // 보간: 가운데 게임 프레임을 가리고 양옆으로 다시 만들어 채점한다 (블록 매칭 보간, RIFE를 쓰는 동안의 채점).
    // 외삽: 앞의 두 게임 프레임으로 다음 프레임을 예측해 채점한다 (블록 매칭 외삽).
    FgLearner interp;
    FgLearner extrap;
    FgLearner& Learner(bool extrapolate) { return extrapolate ? extrap : interp; }
    const FgLearner& Learner(bool extrapolate) const { return extrapolate ? extrap : interp; }

    std::vector<MapCell> map;      // kMapW * kMapH
    int mapSamples = 0;

    // 프레임 생성 영역 (게임 화면 크기에 대한 비율 0~1). 너비가 0이면 화면 전체에 만든다.
    // 화면 지도와 학습은 이 영역 안에서만 이루어진다.
    float regionX = 0, regionY = 0, regionW = 0, regionH = 0;
    bool HasRegion() const { return regionW > 0 && regionH > 0; }
    // 영역을 바꾼다. 화면 지도의 칸 위치가 달라지므로 지도는 처음부터 다시 모은다.
    void SetRegion(float x, float y, float w, float h);

    // 고정 UI 상자 (저장된다). 확대를 시작할 때의 상자(stillBase)에 이번 확대에서 찾은 상자를 합쳐 다시 만든다.
    // 확대 중에 몇 번 합쳐도(1분마다 저장) 확대 한 번에 한 번 합친 것과 같다.
    std::vector<StillBox> stillBoxes;
    std::vector<StillBox> stillBase;
    bool stillSessionBegun = false;
    void BeginStillSession();
    // found: 이번 확대에서 찾은 상자, movingSeconds: 이번 확대에서 화면이 움직인 시간(초), aspect: 게임 화면 가로/세로
    void UpdateStillSession(const std::vector<StillBox>& found, double movingSeconds, float aspect);

    GameProfile();

    // 화면 지도 --------------------------------------------------------------
    // 움직임 격자(xyzw float)를 칸별로 모아 지도를 갱신한다. interval: 진짜 프레임 간격(초)
    void AddFlowSample(const float* flow, int gridW, int gridH, int rowPitchFloats, double interval);
    void UpdateGenre();

    // 마지막으로 확대한 게임 화면 크기. 학습된 칸 크기가 원본 픽셀 단위라 공유할 때 참고로 남긴다.
    int sourceW = 0, sourceH = 0;
    std::string appVersion;        // 마지막으로 저장한 OpenScaler 버전

    bool Save(const std::wstring& dir) const;      // dir 안에 게임 이름으로 저장한다
    bool SaveTo(const std::wstring& path) const;   // 정한 경로에 저장한다 (공유용 내보내기)
    bool Load(const std::wstring& path);           // name은 바꾸지 않는다
    static std::wstring ReadName(const std::wstring& path);  // 파일에 적힌 게임 이름. 없으면 빈 문자열

};

std::wstring ProfilesDirectory();
std::vector<std::wstring> ListProfiles(const std::wstring& dir);
std::wstring ProfilePath(const std::wstring& dir, const std::wstring& name);
