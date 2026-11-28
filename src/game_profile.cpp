#include "game_profile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

const char* const kGenreKeys[GENRE_COUNT] = { "general", "rhythm", "scroll" };

namespace {

// 값마다 시험해 볼 후보들
const int kCellOptions[] = { 4, 8, 16 };
const int kRadiusOptions[] = { 8, 12, 16 };
const float kSmoothOptions[] = { 0.0005f, 0.0015f, 0.004f };
const float kTolOptions[] = { 0.04f, 0.08f, 0.14f };
const int kAxisCount = 5;
const int kOptionsPerAxis = 3;

const int kPairsPerTrial = 6;       // 후보 하나를 이 횟수만큼 짝지어 채점한 뒤 결정한다
const float kAdoptMargin = 0.05f;   // 평균이 이만큼(dB) 좋아야 바꾼다. 측정 오차로 값이 흔들리지 않게 한다
const int kDefaultCheckPeriod = 3;  // 기본값과 다른 설정을 쓰는 동안 이 횟수마다 한 번은 기본값을 후보로 시험한다
const float kCellCostMargin = 0.15f;  // 칸을 더 작게 하면 계산이 늘어나므로 더 큰 향상을 요구한다
const int kEvalsPerHistoryPoint = 20;
const size_t kMaxHistory = 240;
const float kMovingShare = 0.08f;   // 칸의 이만큼이 움직이면 움직이는 칸으로 본다

FgParams WithOption(FgParams p, int axis, int option)
{
    switch (axis) {
    case 0: p.cellPx = kCellOptions[option]; break;
    case 1: p.coarseRadius = kRadiusOptions[option]; break;
    case 2: p.smoothness = kSmoothOptions[option]; break;
    case 3: p.blendTol = kTolOptions[option]; break;
    case 4: p.useMap = option == 1; break;
    }
    return p;
}

// 우리가 쓴 형식만 읽는 작은 파서. 없는 값이나 잘못된 값은 기본값으로 둔다.
const char* FindKey(const std::string& text, const char* key)
{
    std::string needle = std::string("\"") + key + "\":";
    size_t pos = text.find(needle);
    return pos == std::string::npos ? nullptr : text.c_str() + pos + needle.size();
}

double ReadNumber(const std::string& text, const char* key, double fallback)
{
    const char* p = FindKey(text, key);
    if (!p) return fallback;
    char* end = nullptr;
    double v = strtod(p, &end);
    return end == p || !std::isfinite(v) ? fallback : v;
}

std::vector<float> ReadArray(const std::string& text, const char* key)
{
    std::vector<float> out;
    const char* p = FindKey(text, key);
    if (!p) return out;
    while (*p == ' ') p++;
    if (*p != '[') return out;
    p++;
    while (*p && *p != ']') {
        char* end = nullptr;
        double v = strtod(p, &end);
        if (end == p) { p++; continue; }
        out.push_back(std::isfinite(v) ? (float)v : 0.0f);
        p = end;
    }
    return out;
}

// 따옴표 안의 문자열 값. 이스케이프는 \" 와 \\ 만 쓴다.
std::string ReadString(const std::string& text, const char* key)
{
    const char* p = FindKey(text, key);
    if (!p) return {};
    while (*p == ' ') p++;
    if (*p != '"') return {};
    std::string out;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        out += *p;
    }
    return out;
}

std::string EscapeJson(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

const int kGainPoints = kShareMinEvals / kEvalsPerHistoryPoint;  // 개선 dB를 평균 낼 그래프 점 수

void WriteArray(std::ostringstream& os, const char* key, const std::vector<float>& v, bool last = false)
{
    os << "  \"" << key << "\": [";
    for (size_t i = 0; i < v.size(); i++) os << (i ? "," : "") << v[i];
    os << "]" << (last ? "\n" : ",\n");
}

}  // namespace

GameProfile::GameProfile() : map(kMapW * kMapH)
{
    interp.Start(mapSamples);
    extrap.Start(mapSamples);
}

void GameProfile::SetRegion(float x, float y, float w, float h)
{
    if (x == regionX && y == regionY && w == regionW && h == regionH) return;
    regionX = x;
    regionY = y;
    regionW = w;
    regionH = h;
    map.assign(map.size(), MapCell{});
    mapSamples = 0;
    genre = GENRE_GENERAL;
}

float FgLearner::RecentGain(int& evals) const
{
    const size_t n = std::min({ history.size(), baseline.size(), (size_t)kGainPoints });
    evals = (int)n * kEvalsPerHistoryPoint;
    if (n == 0) return 0;
    double sum = 0;
    for (size_t i = history.size() - n; i < history.size(); i++) sum += history[i] - baseline[i];
    return (float)(sum / n);
}

void FgLearner::Start(int mapSamples)
{
    NextCandidate(mapSamples);
}

void FgLearner::NextCandidate(int mapSamples)
{
    m_pairs = 0;
    m_deltas.clear();

    // 다른 화면(메뉴, 결과 화면 등)에서 고른 값이 게임 화면에서는 나쁠 수 있으므로, 기본값으로 돌아갈 기회를 준다.
    if (++m_trials % kDefaultCheckPeriod == 0) {
        FgParams defaults;
        defaults.useMap = params.useMap;  // 지도는 따로 시험한다
        if (!(defaults == params)) {
            m_candidate = defaults;
            return;
        }
    }

    // 값을 하나씩 돌아가며, 지금과 다른 후보를 하나씩 시험한다.
    for (int tries = 0; tries < kAxisCount * kOptionsPerAxis + 1; tries++) {
        m_step++;
        if (m_step >= kOptionsPerAxis) {
            m_step = 0;
            m_axis = (m_axis + 1) % kAxisCount;
        }
        // 지도는 표본이 쌓인 뒤에만 시험한다.
        if (m_axis == 4 && (mapSamples < 20 || m_step > 1)) continue;
        FgParams next = WithOption(params, m_axis, m_step);
        if (!(next == params)) {
            m_candidate = next;
            break;
        }
    }
}

void FgLearner::AddEvaluation(float currentDb, float candidateDb, float baselineDb, int mapSamples)
{
    evalCount++;
    bestPsnr = bestPsnr <= 0 ? currentDb : bestPsnr * 0.9f + currentDb * 0.1f;

    m_sumCurrent += currentDb;
    m_sumBaseline += baselineDb;
    if (++m_historyCount >= kEvalsPerHistoryPoint) {
        history.push_back(m_sumCurrent / m_historyCount);
        baseline.push_back(m_sumBaseline / m_historyCount);
        if (history.size() > kMaxHistory) {
            history.erase(history.begin());
            baseline.erase(baseline.begin());
        }
        m_sumCurrent = m_sumBaseline = 0;
        m_historyCount = 0;
    }

    m_deltas.push_back(candidateDb - currentDb);
    if (++m_pairs < kPairsPerTrial) return;

    // 기본값으로 돌아가는 것은 손해를 막는 쪽이므로 계산량과 상관없이 조금이라도 나으면 돌아간다.
    // 평균은 한 화면에서의 큰 차이에 끌려가므로 이때는 중앙값과 이긴 비율로 본다.
    FgParams defaults;
    defaults.useMap = params.useMap;
    bool adopt;
    if (m_candidate == defaults) {
        std::vector<float> sorted = m_deltas;
        std::sort(sorted.begin(), sorted.end());
        const float median = 0.5f * (sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]);
        const int wins = (int)std::count_if(m_deltas.begin(), m_deltas.end(), [](float d) { return d > 0; });
        adopt = median > 0 && wins * 2 >= m_pairs;
    } else {
        float sum = 0;
        for (float d : m_deltas) sum += d;
        const float margin = m_candidate.cellPx < params.cellPx ? kCellCostMargin : kAdoptMargin;
        adopt = sum / m_pairs > margin;
    }
    if (adopt) {
        params = m_candidate;
        adoptedCount++;
    }
    NextCandidate(mapSamples);
}

void GameProfile::AddFlowSample(const float* flow, int gridW, int gridH, int rowPitchFloats, double interval)
{
    if (interval <= 0) return;

    // 칸마다 가장 많이 나온 움직임(4픽셀 단위로 묶음)을 구한다.
    struct TileStat {
        std::map<int, int> bins;
        std::map<int, std::pair<float, float>> binSums;  // 묶음 안의 실제 벡터 합 (지도에는 평균을 저장한다)
        int count = 0, moving = 0;
    };
    std::vector<TileStat> tiles(kMapW * kMapH);
    const int binPx = 4;
    int movingCells = 0;

    for (int y = 0; y < gridH; y++) {
        const float* row = flow + (size_t)y * rowPitchFloats;
        int ty = std::min(y * kMapH / gridH, kMapH - 1);
        for (int x = 0; x < gridW; x++) {
            float vx = row[x * 4], vy = row[x * 4 + 1];
            if (row[x * 4 + 3] < 0.3f) continue;  // 믿을 수 없는 벡터는 세지 않는다
            int tx = std::min(x * kMapW / gridW, kMapW - 1);
            TileStat& t = tiles[(size_t)ty * kMapW + tx];
            int bx = (int)std::lround(vx / binPx), by = (int)std::lround(vy / binPx);
            int key = ((by + 4096) << 13) | (bx + 4096);
            t.bins[key]++;
            auto& binSum = t.binSums[key];
            binSum.first += vx;
            binSum.second += vy;
            t.count++;
            if (bx != 0 || by != 0) {
                t.moving++;
                movingCells++;
            }
        }
    }
    if (movingCells == 0) return;  // 멈춘 장면은 고정 UI 판단에 쓰지 않는다

    const float rate = 0.05f;
    for (int i = 0; i < kMapW * kMapH; i++) {
        const TileStat& t = tiles[i];
        MapCell& m = map[i];
        if (t.count == 0) continue;

        // 노트처럼 칸의 일부만 움직여도 알아볼 수 있도록, 멈춘 벡터를 뺀 나머지에서 가장 많은 움직임을 찾는다.
        const int zeroKey = (4096 << 13) | 4096;
        int bestKey = zeroKey, bestCount = 0;
        for (auto& [key, n] : t.bins)
            if (key != zeroKey && n > bestCount) {
                bestCount = n;
                bestKey = key;
            }
        bool moving = t.moving > 0 && (float)t.moving / t.count >= kMovingShare;
        float vx = 0, vy = 0;
        if (moving) {
            auto binSum = t.binSums.at(bestKey);
            vx = binSum.first / bestCount;
            vy = binSum.second / bestCount;
        }

        m.activity += ((moving ? 1.0f : 0.0f) - m.activity) * rate;
        m.stillness += ((moving ? 0.0f : 1.0f) - m.stillness) * rate;
        if (moving) {
            m.coherence += ((float)bestCount / t.moving - m.coherence) * rate;
            // 프레임 간격이 달라져도 쓸 수 있게 초당 픽셀로 저장한다.
            float sx = vx / (float)interval, sy = vy / (float)interval;
            float mix = m.activity < 0.05f ? 0.5f : rate;
            m.vx += (sx - m.vx) * mix;
            m.vy += (sy - m.vy) * mix;
        }
    }
    mapSamples++;
}

void GameProfile::UpdateGenre()
{
    if (mapSamples < 20) return;

    // 한 방향으로 흐르는 영역과 멈춘 영역이 함께 있으면 리듬게임형,
    // 화면 대부분이 한 방향으로 흐르면 스크롤형, 나머지는 일반으로 본다.
    int active = 0, still = 0;
    float dirX = 0, dirY = 0;
    for (const MapCell& m : map) {
        if (m.stillness > 0.8f) still++;
        if (m.activity > 0.3f && m.coherence > 0.5f) {
            active++;
            float len = std::sqrt(m.vx * m.vx + m.vy * m.vy);
            if (len > 1e-3f) {
                dirX += m.vx / len;
                dirY += m.vy / len;
            }
        }
    }
    const float total = (float)map.size();
    float activeShare = active / total, stillShare = still / total;
    float agreement = active ? std::sqrt(dirX * dirX + dirY * dirY) / active : 0;

    if (agreement > 0.8f && activeShare > 0.03f && activeShare < 0.7f && stillShare > 0.25f) genre = GENRE_RHYTHM;
    else if (agreement > 0.8f && activeShare >= 0.7f) genre = GENRE_SCROLL;
    else genre = GENRE_GENERAL;
}

void GameProfile::BeginStillSession()
{
    stillBase = stillBoxes;
    stillSessionBegun = true;
}

static float BoxIou(const StillBox& a, const StillBox& b)
{
    const float ix = std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    const float iy = std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    const float inter = ix * iy, uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0;
}

// a의 넓이 중 boxes가 덮은 비율. 이번에 찾은 상자끼리는 겹치지 않으므로 겹친 넓이를 더해 1로 자른다.
static float BoxCoverage(const StillBox& a, const std::vector<StillBox>& boxes)
{
    const float area = a.w * a.h;
    if (area <= 0) return 0;
    float inter = 0;
    for (const StillBox& b : boxes) {
        const float ix = std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
        const float iy = std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
        inter += ix * iy;
    }
    return std::min(1.0f, inter / area);
}

void GameProfile::UpdateStillSession(const std::vector<StillBox>& found, double movingSeconds, float aspect)
{
    // 화면이 거의 움직이지 않았으면 고정인지 가릴 수 없으므로 시작할 때 그대로 둔다.
    if (movingSeconds < kStillBoxMinMoving) {
        stillBoxes = stillBase;
        return;
    }
    std::vector<StillBox> out;
    std::vector<bool> used(found.size(), false);
    for (const StillBox& b : stillBase) {
        int best = -1;
        float bestIou = 0.4f;  // 이만큼 겹쳐야 같은 상자로 본다
        for (size_t i = 0; i < found.size(); i++) {
            const float iou = BoxIou(b, found[i]);
            if (iou >= bestIou) {
                bestIou = iou;
                best = (int)i;
            }
        }
        StillBox m = b;
        if (best >= 0) {
            // 확신도가 높을수록 기존 위치를 믿는다. 해상도나 UI 배율이 바뀌어 조금 어긋났으면 점점 옮겨 간다.
            const StillBox& f = found[best];
            const float wb = b.confidence / (b.confidence + 1.0f);
            m.x = b.x * wb + f.x * (1 - wb);
            m.y = b.y * wb + f.y * (1 - wb);
            m.w = b.w * wb + f.w * (1 - wb);
            m.h = b.h * wb + f.h * (1 - wb);
            m.confidence = std::min(b.confidence + 1.0f, kStillBoxMaxConfidence);
            m.aspect = aspect;
            used[best] = true;
        } else if (BoxCoverage(b, found) >= kStillBoxCover) {
            // 이웃 상자와 붙어 큰 상자로 찾았거나 여러 조각으로 나뉘어 찾았어도 그 자리가 고정이면 확인된 것으로 본다.
            // 상자 모양이 달라 위치는 옮기지 않는다.
            m.confidence = std::min(b.confidence + 1.0f, kStillBoxMaxConfidence);
            m.aspect = aspect;
        } else {
            m.confidence = b.confidence - 1.0f;
        }
        if (m.confidence > 0) out.push_back(m);
    }
    for (size_t i = 0; i < found.size(); i++) {
        if (used[i]) continue;
        // 대부분이 기존 상자 자리면 기존 상자들이 붙거나 나뉜 것이므로 새 상자로 더하지 않는다.
        if (BoxCoverage(found[i], stillBase) >= kStillBoxCover) continue;
        StillBox m = found[i];
        m.confidence = 1.0f;
        m.aspect = aspect;
        out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const StillBox& a, const StillBox& b) {
        return a.confidence != b.confidence ? a.confidence > b.confidence : a.w * a.h > b.w * b.h;
    });
    if (out.size() > (size_t)kStillBoxMax) out.resize(kStillBoxMax);
    stillBoxes = out;
}

bool GameProfile::Save(const std::wstring& dir) const
{
    CreateDirectoryW(dir.c_str(), nullptr);
    return SaveTo(ProfilePath(dir, name));
}

std::wstring GameProfile::ReadName(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return FromUtf8(ReadString(ss.str(), "name"));
}

bool GameProfile::SaveTo(const std::wstring& path) const
{
    std::vector<float> mapValues;
    mapValues.reserve(map.size() * 5);
    for (const MapCell& m : map) {
        mapValues.push_back(m.vx);
        mapValues.push_back(m.vy);
        mapValues.push_back(m.activity);
        mapValues.push_back(m.stillness);
        mapValues.push_back(m.coherence);
    }

    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": 1,\n";
    os << "  \"name\": \"" << EscapeJson(ToUtf8(name)) << "\",\n";
    os << "  \"app\": \"" << OPENSCALER_VERSION << "\",\n";
    os << "  \"sourceW\": " << sourceW << ",\n";
    os << "  \"sourceH\": " << sourceH << ",\n";
    os << "  \"genre\": " << genre << ",\n";
    // 보간 학습은 예전 파일과 같은 이름으로, 외삽 학습은 x_를 붙여 쓴다.
    for (int k = 0; k < 2; k++) {
        const FgLearner& l = k == 0 ? interp : extrap;
        const std::string pre = k == 0 ? "" : "x_";
        os << "  \"" << pre << "cellPx\": " << l.params.cellPx << ",\n";
        os << "  \"" << pre << "coarseRadius\": " << l.params.coarseRadius << ",\n";
        os << "  \"" << pre << "smoothness\": " << l.params.smoothness << ",\n";
        os << "  \"" << pre << "blendTol\": " << l.params.blendTol << ",\n";
        os << "  \"" << pre << "useMap\": " << (l.params.useMap ? 1 : 0) << ",\n";
        os << "  \"" << pre << "evalCount\": " << l.evalCount << ",\n";
        os << "  \"" << pre << "adoptedCount\": " << l.adoptedCount << ",\n";
        os << "  \"" << pre << "bestPsnr\": " << l.bestPsnr << ",\n";
        WriteArray(os, (pre + "history").c_str(), l.history);
        WriteArray(os, (pre + "baseline").c_str(), l.baseline);
    }
    os << "  \"regionX\": " << regionX << ",\n";
    os << "  \"regionY\": " << regionY << ",\n";
    os << "  \"regionW\": " << regionW << ",\n";
    os << "  \"regionH\": " << regionH << ",\n";
    std::vector<float> boxValues;
    for (const StillBox& b : stillBoxes) boxValues.insert(boxValues.end(), { b.x, b.y, b.w, b.h, b.confidence, b.aspect });
    WriteArray(os, "stillBoxes", boxValues);  // 상자마다 x, y, 너비, 높이, 확신도, 화면 비율
    os << "  \"mapSamples\": " << mapSamples << ",\n";
    WriteArray(os, "map", mapValues, true);
    os << "}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << os.str();
    return file.good();
}

bool GameProfile::Load(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (FindKey(text, "version") == nullptr) return false;

    // 파일을 직접 고쳤거나 깨졌어도 후보 목록 안의 값만 받아들인다.
    auto nearestInt = [](double v, const int* options, int n) {
        int best = options[0];
        for (int i = 1; i < n; i++)
            if (std::abs(options[i] - v) < std::abs(best - v)) best = options[i];
        return best;
    };
    auto nearestFloat = [](double v, const float* options, int n) {
        float best = options[0];
        for (int i = 1; i < n; i++)
            if (std::abs(options[i] - v) < std::abs(best - v)) best = options[i];
        return best;
    };
    for (int k = 0; k < 2; k++) {
        FgLearner& l = k == 0 ? interp : extrap;
        const std::string pre = k == 0 ? "" : "x_";
        auto key = [&](const char* n) { return pre + n; };
        l.params.cellPx = nearestInt(ReadNumber(text, key("cellPx").c_str(), l.params.cellPx), kCellOptions, kOptionsPerAxis);
        l.params.coarseRadius = nearestInt(ReadNumber(text, key("coarseRadius").c_str(), l.params.coarseRadius), kRadiusOptions, kOptionsPerAxis);
        l.params.smoothness = nearestFloat(ReadNumber(text, key("smoothness").c_str(), l.params.smoothness), kSmoothOptions, kOptionsPerAxis);
        l.params.blendTol = nearestFloat(ReadNumber(text, key("blendTol").c_str(), l.params.blendTol), kTolOptions, kOptionsPerAxis);
        l.params.useMap = ReadNumber(text, key("useMap").c_str(), 0) != 0;
        l.evalCount = std::max(0, (int)ReadNumber(text, key("evalCount").c_str(), 0));
        l.adoptedCount = std::max(0, (int)ReadNumber(text, key("adoptedCount").c_str(), 0));
        l.bestPsnr = (float)ReadNumber(text, key("bestPsnr").c_str(), 0);
        l.history = ReadArray(text, key("history").c_str());
        l.baseline = ReadArray(text, key("baseline").c_str());
        l.baseline.resize(l.history.size());
    }
    genre = std::clamp((int)ReadNumber(text, "genre", genre), 0, GENRE_COUNT - 1);
    mapSamples = std::max(0, (int)ReadNumber(text, "mapSamples", 0));
    sourceW = std::max(0, (int)ReadNumber(text, "sourceW", 0));
    sourceH = std::max(0, (int)ReadNumber(text, "sourceH", 0));
    appVersion = ReadString(text, "app");
    regionX = std::clamp((float)ReadNumber(text, "regionX", 0), 0.0f, 1.0f);
    regionY = std::clamp((float)ReadNumber(text, "regionY", 0), 0.0f, 1.0f);
    regionW = std::clamp((float)ReadNumber(text, "regionW", 0), 0.0f, 1.0f - regionX);
    regionH = std::clamp((float)ReadNumber(text, "regionH", 0), 0.0f, 1.0f - regionY);

    stillBoxes.clear();
    std::vector<float> boxValues = ReadArray(text, "stillBoxes");
    for (size_t i = 0; i + 6 <= boxValues.size() && stillBoxes.size() < (size_t)kStillBoxMax; i += 6) {
        StillBox b;
        b.x = std::clamp(boxValues[i], 0.0f, 1.0f);
        b.y = std::clamp(boxValues[i + 1], 0.0f, 1.0f);
        b.w = std::clamp(boxValues[i + 2], 0.0f, 1.0f - b.x);
        b.h = std::clamp(boxValues[i + 3], 0.0f, 1.0f - b.y);
        b.confidence = std::clamp(boxValues[i + 4], 0.0f, kStillBoxMaxConfidence);
        b.aspect = std::clamp(boxValues[i + 5], 0.0f, 10.0f);
        if (b.w > 0 && b.h > 0 && b.confidence > 0) stillBoxes.push_back(b);
    }
    stillBase.clear();
    stillSessionBegun = false;

    std::vector<float> mapValues = ReadArray(text, "map");
    if (mapValues.size() == map.size() * 5) {
        for (size_t i = 0; i < map.size(); i++) {
            map[i].vx = mapValues[i * 5];
            map[i].vy = mapValues[i * 5 + 1];
            map[i].activity = mapValues[i * 5 + 2];
            map[i].stillness = mapValues[i * 5 + 3];
            map[i].coherence = mapValues[i * 5 + 4];
        }
    }
    interp.Start(mapSamples);
    extrap.Start(mapSamples);
    return true;
}

std::wstring ProfilesDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"profiles";
}

std::vector<std::wstring> ListProfiles(const std::wstring& dir)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        std::wstring n = fd.cFileName;
        names.push_back(n.substr(0, n.size() - 5));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

std::wstring ProfilePath(const std::wstring& dir, const std::wstring& name)
{
    // 파일 이름에 쓸 수 없는 글자는 밑줄로 바꾼다.
    std::wstring safe = name;
    for (wchar_t& c : safe)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return dir + L"\\" + safe + L".json";
}
