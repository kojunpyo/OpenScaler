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
const float kAdoptMargin = 0.05f;   // 이만큼(dB) 좋아야 바꾼다. 측정 오차로 값이 흔들리지 않게 한다
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

void WriteArray(std::ostringstream& os, const char* key, const std::vector<float>& v, bool last = false)
{
    os << "  \"" << key << "\": [";
    for (size_t i = 0; i < v.size(); i++) os << (i ? "," : "") << v[i];
    os << "]" << (last ? "\n" : ",\n");
}

}  // namespace

GameProfile::GameProfile() : map(kMapW * kMapH)
{
    NextCandidate();
}

void GameProfile::NextCandidate()
{
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
    m_pairs = 0;
    m_deltaSum = 0;
}

void GameProfile::AddEvaluation(float currentDb, float candidateDb, float blendDb)
{
    evalCount++;
    bestPsnr = bestPsnr <= 0 ? currentDb : bestPsnr * 0.9f + currentDb * 0.1f;

    m_sumCurrent += currentDb;
    m_sumBlend += blendDb;
    if (++m_historyCount >= kEvalsPerHistoryPoint) {
        history.push_back(m_sumCurrent / m_historyCount);
        baseline.push_back(m_sumBlend / m_historyCount);
        if (history.size() > kMaxHistory) {
            history.erase(history.begin());
            baseline.erase(baseline.begin());
        }
        m_sumCurrent = m_sumBlend = 0;
        m_historyCount = 0;
    }

    m_deltaSum += candidateDb - currentDb;
    if (++m_pairs < kPairsPerTrial) return;

    float margin = m_candidate.cellPx < params.cellPx ? kCellCostMargin : kAdoptMargin;
    if (m_deltaSum / m_pairs > margin) {
        params = m_candidate;
        adoptedCount++;
    }
    NextCandidate();
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

bool GameProfile::Save(const std::wstring& dir) const
{
    CreateDirectoryW(dir.c_str(), nullptr);

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
    os << "  \"genre\": " << genre << ",\n";
    os << "  \"cellPx\": " << params.cellPx << ",\n";
    os << "  \"coarseRadius\": " << params.coarseRadius << ",\n";
    os << "  \"smoothness\": " << params.smoothness << ",\n";
    os << "  \"blendTol\": " << params.blendTol << ",\n";
    os << "  \"useMap\": " << (params.useMap ? 1 : 0) << ",\n";
    os << "  \"evalCount\": " << evalCount << ",\n";
    os << "  \"adoptedCount\": " << adoptedCount << ",\n";
    os << "  \"bestPsnr\": " << bestPsnr << ",\n";
    os << "  \"mapSamples\": " << mapSamples << ",\n";
    WriteArray(os, "history", history);
    WriteArray(os, "baseline", baseline);
    WriteArray(os, "map", mapValues, true);
    os << "}\n";

    std::ofstream file(ProfilePath(dir, name), std::ios::binary | std::ios::trunc);
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
    params.cellPx = nearestInt(ReadNumber(text, "cellPx", params.cellPx), kCellOptions, kOptionsPerAxis);
    params.coarseRadius = nearestInt(ReadNumber(text, "coarseRadius", params.coarseRadius), kRadiusOptions, kOptionsPerAxis);
    params.smoothness = nearestFloat(ReadNumber(text, "smoothness", params.smoothness), kSmoothOptions, kOptionsPerAxis);
    params.blendTol = nearestFloat(ReadNumber(text, "blendTol", params.blendTol), kTolOptions, kOptionsPerAxis);
    params.useMap = ReadNumber(text, "useMap", 0) != 0;
    genre = std::clamp((int)ReadNumber(text, "genre", genre), 0, GENRE_COUNT - 1);
    evalCount = std::max(0, (int)ReadNumber(text, "evalCount", 0));
    adoptedCount = std::max(0, (int)ReadNumber(text, "adoptedCount", 0));
    bestPsnr = (float)ReadNumber(text, "bestPsnr", 0);
    mapSamples = std::max(0, (int)ReadNumber(text, "mapSamples", 0));
    history = ReadArray(text, "history");
    baseline = ReadArray(text, "baseline");
    baseline.resize(history.size());

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
    NextCandidate();
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
