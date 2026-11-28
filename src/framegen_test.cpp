// 프레임 생성 자체 테스트 (--fg-selftest 폴더)
//
// 계산으로 만든 움직이는 영상에서 가운데 프레임을 빼고 양옆 프레임으로 다시 만든 뒤 실제 프레임과 PSNR로 비교한다.
// 화면 캡처나 마우스 없이 GPU만 쓰므로 언제든 같은 결과를 얻을 수 있다.

#include "framegen_test.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ppl.h>
#include <functional>
#include <string>
#include <vector>

#include "framegen.h"
#include "rife.h"

namespace {

// 결과 폴더를 상위 폴더까지 만든다. 없는 상위 폴더를 주면 결과 파일이 조용히 저장되지 않았다.
void MakeDirectories(const std::wstring& dir)
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);
}

// 이음매 없는 값 노이즈 (게임 배경 대신 쓰는 무늬)
struct NoiseTexture {
    static constexpr int kSize = 512;
    std::vector<float> values;

    NoiseTexture()
    {
        values.assign((size_t)kSize * kSize, 0.0f);
        uint32_t seed = 12345;
        auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / float(1 << 24);
        };
        for (int period : { 64, 32, 16, 8, 4 }) {
            int n = kSize / period;
            std::vector<float> lattice((size_t)n * n);
            for (float& v : lattice) v = rnd();
            float amp = period / 124.0f;
            for (int y = 0; y < kSize; y++) {
                for (int x = 0; x < kSize; x++) {
                    float fx = (float)x / period, fy = (float)y / period;
                    int ix = (int)fx, iy = (int)fy;
                    float ax = fx - ix, ay = fy - iy;
                    ax = ax * ax * (3 - 2 * ax);
                    ay = ay * ay * (3 - 2 * ay);
                    float v00 = lattice[(size_t)iy % n * n + ix % n];
                    float v10 = lattice[(size_t)iy % n * n + (ix + 1) % n];
                    float v01 = lattice[(size_t)(iy + 1) % n * n + ix % n];
                    float v11 = lattice[(size_t)(iy + 1) % n * n + (ix + 1) % n];
                    values[(size_t)y * kSize + x] +=
                        amp * ((v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay);
                }
            }
        }
    }

    float At(float x, float y) const
    {
        float fx = x - std::floor(x / kSize) * kSize;
        float fy = y - std::floor(y / kSize) * kSize;
        int ix = (int)fx % kSize, iy = (int)fy % kSize;
        float ax = fx - std::floor(fx), ay = fy - std::floor(fy);
        int jx = (ix + 1) % kSize, jy = (iy + 1) % kSize;
        float a = values[(size_t)iy * kSize + ix] * (1 - ax) + values[(size_t)iy * kSize + jx] * ax;
        float b = values[(size_t)jy * kSize + ix] * (1 - ax) + values[(size_t)jy * kSize + jx] * ax;
        return a * (1 - ay) + b * ay;
    }
};

struct Rgb {
    float r, g, b;
};

Rgb Textured(const NoiseTexture& n, float x, float y, Rgb tint)
{
    float v = std::clamp(n.At(x, y), 0.0f, 1.0f);
    return { v * tint.r, v * tint.g, v * tint.b };
}

// 장면: 프레임 번호(소수 가능)와 좌표(비율)로 색을 돌려준다. x, y는 원본 픽셀.
using Scene = std::function<Rgb(float frame, float x, float y, int w, int h)>;

struct SceneInfo {
    const char* key;
    const char* note;
    Scene shade;
};

float Box(float x, float y, float x0, float y0, float x1, float y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1 ? 1.0f : 0.0f;
}

std::vector<SceneInfo> MakeScenes(const NoiseTexture& noise)
{
    const NoiseTexture* n = &noise;
    std::vector<SceneInfo> scenes;

    // 화면 전체가 한 방향으로 움직인다
    scenes.push_back({ "pan", "전체 이동", [n](float f, float x, float y, int, int) {
        return Textured(*n, x + f * 9.0f, y + f * 3.0f, { 1.0f, 0.9f, 0.8f });
    } });

    // 움직이는 배경 위에 고정 UI
    scenes.push_back({ "pan_ui", "이동 + 고정 UI", [n](float f, float x, float y, int w, int h) {
        Rgb c = Textured(*n, x + f * 9.0f, y, { 0.9f, 1.0f, 0.9f });
        bool panel = x > w * 0.04f && x < w * 0.34f && y > h * 0.82f && y < h * 0.95f;
        if (panel) {
            bool glyph = ((int)x / 6 + (int)y / 6) % 2 == 0;
            return glyph ? Rgb{ 1, 1, 1 } : Rgb{ 0.08f, 0.1f, 0.15f };
        }
        return c;
    } });

    // 느린 배경 위를 지나가는 물체들 (가림과 드러남)
    scenes.push_back({ "sprites", "물체 + 가림", [n](float f, float x, float y, int w, int h) {
        Rgb c = Textured(*n, x + f * 2.0f, y, { 0.7f, 0.75f, 0.9f });
        struct { float x0, y0, vx, vy, r; Rgb tint; } objs[] = {
            { 0.10f, 0.30f, 14.0f, 0.0f, 0.10f, { 1.0f, 0.6f, 0.4f } },
            { 0.85f, 0.65f, -20.0f, 2.0f, 0.07f, { 0.5f, 1.0f, 0.6f } },
            { 0.45f, 0.10f, 3.0f, 12.0f, 0.05f, { 1.0f, 1.0f, 0.5f } },
        };
        for (auto& o : objs) {
            // 화면 밖으로 나가면 반대쪽에서 다시 들어온다 (시험 구간 어디서나 물체가 보이게)
            float spanX = w * 1.5f, spanY = h * 1.5f;
            float cx = std::fmod(std::fmod(o.x0 * w + o.vx * f, spanX) + spanX, spanX) - w * 0.25f;
            float cy = std::fmod(std::fmod(o.y0 * h + o.vy * f, spanY) + spanY, spanY) - h * 0.25f;
            float r = o.r * h;
            float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy < r * r) return Textured(*n, dx + 100, dy + 100, o.tint);
        }
        return c;
    } });

    // 리듬게임: 어두운 레인을 작은 노트가 빠르게 내려오고, 판정선과 점수는 고정
    scenes.push_back({ "rhythm", "작고 빠른 노트", [](float f, float x, float y, int w, int h) {
        Rgb c = { 0.05f, 0.05f, 0.09f };
        const float laneLeft = w * 0.3f, laneW = w * 0.1f;
        const float noteH = h * 0.03f, speed = h * 0.06f;
        if (x > laneLeft && x < laneLeft + laneW * 4) {
            int lane = (int)((x - laneLeft) / laneW);
            if ((x - laneLeft) - lane * laneW < 2) c = { 0.3f, 0.3f, 0.35f };
            static const float offsets[4][4] = { { 0.0f, 0.42f, 0.9f, 1.4f }, { 0.25f, 0.7f, 1.1f, 1.7f },
                                                 { 0.12f, 0.55f, 1.25f, 1.6f }, { 0.33f, 0.8f, 1.0f, 1.9f } };
            for (float o : offsets[std::min(lane, 3)]) {
                float period = h * 2.0f;
                float top = std::fmod(f * speed + o * h, period) - h * 0.2f;
                if (y >= top && y < top + noteH && (x - laneLeft) - lane * laneW > 3 && (x - laneLeft) - lane * laneW < laneW - 3)
                    c = lane == 1 || lane == 2 ? Rgb{ 1.0f, 0.35f, 0.6f } : Rgb{ 0.5f, 0.85f, 1.0f };
            }
            if (Box(x, y, laneLeft, h * 0.84f, laneLeft + laneW * 4, h * 0.85f + 2)) c = { 1, 1, 1 };
        }
        // 점수 칸
        if (x > w * 0.75f && x < w * 0.95f && y > h * 0.06f && y < h * 0.14f)
            return ((int)x / 4 + (int)y / 4) % 2 == 0 ? Rgb{ 0.9f, 0.9f, 0.9f } : Rgb{ 0.1f, 0.1f, 0.1f };
        return c;
    } });

    // 빠른 전체 이동 (탐색 범위 시험)
    scenes.push_back({ "fast_pan", "빠른 이동", [n](float f, float x, float y, int, int) {
        return Textured(*n, x + f * 42.0f, y - f * 11.0f, { 0.9f, 0.85f, 1.0f });
    } });

    // 움직임 없이 밝기만 변한다 (보간이 망가뜨리면 안 되는 경우)
    scenes.push_back({ "fade", "밝기 변화", [n](float f, float x, float y, int, int) {
        float gain = 0.55f + 0.4f * std::sin(f * 0.35f);
        return Textured(*n, x, y, { gain, gain, gain });
    } });

    return scenes;
}

// 1.6.0 처리를 시험하는 장면 (앞의 표들과 섞이지 않게 따로 둔다)
std::vector<SceneInfo> MakeExtraScenes(const NoiseTexture& noise)
{
    const NoiseTexture* n = &noise;
    std::vector<SceneInfo> scenes;

    // 움직이는 배경 위의 얇은 조준점과 글자 (배경이 비쳐 보이는 작은 고정 UI)
    scenes.push_back({ "crosshair", "조준점 + 얇은 글자", [n](float f, float x, float y, int w, int h) {
        Rgb c = Textured(*n, x + f * 9.0f, y + f * 3.0f, { 0.9f, 0.9f, 1.0f });
        const float cx = w * 0.5f, cy = h * 0.5f;
        const bool arm = (std::abs(x - cx) < 1.5f && std::abs(y - cy) < 14.0f && std::abs(y - cy) > 4.0f) ||
                         (std::abs(y - cy) < 1.5f && std::abs(x - cx) < 14.0f && std::abs(x - cx) > 4.0f);
        if (arm) return Rgb{ 1, 1, 1 };
        // 왼쪽 위 글자: 3픽셀 획의 글자 모양만 칠하고 사이로 배경이 보인다
        if (x > w * 0.05f && x < w * 0.25f && y > h * 0.06f && y < h * 0.11f) {
            const int gx = (int)((x - w * 0.05f) / 3), gy = (int)((y - h * 0.06f) / 3);
            if ((gx % 4 == 0) || (gy % 5 == 0 && gx % 4 < 3)) return Rgb{ 1, 0.95f, 0.4f };
        }
        return c;
    } });

    // 제자리에서 밝기와 대비가 크게 바뀌는 글자 (리듬게임의 판정 글자)
    scenes.push_back({ "flash_text", "깜빡이는 글자", [n](float f, float x, float y, int w, int h) {
        Rgb c = Textured(*n, x + f * 2.0f, y, { 0.25f, 0.25f, 0.3f });
        if (x > w * 0.3f && x < w * 0.7f && y > h * 0.4f && y < h * 0.6f) {
            const float gain = 0.2f + 0.8f * std::abs(std::sin(f * 0.9f));
            const int gx = (int)((x - w * 0.3f) / 8), gy = (int)((y - h * 0.4f) / 8);
            const bool glyph = (gx % 3 != 2) && ((gy % 4 == 0) || (gx % 3 == 0) || ((gx + gy) % 5 == 0));
            if (glyph) return Rgb{ c.r + gain * (1.0f - c.r), c.g + gain * (0.8f - c.g), c.b + gain * (0.3f - c.b) };
            return Rgb{ c.r * (1.0f - 0.5f * gain), c.g * (1.0f - 0.5f * gain), c.b * (1.0f - 0.5f * gain) };
        }
        return c;
    } });

    // 장면 전환: 프레임 10과 11 사이에서 전혀 다른 장면으로 바뀐다
    scenes.push_back({ "cut", "장면 전환", [n](float f, float x, float y, int, int) {
        if (f < 10.5f) return Textured(*n, x + f * 9.0f, y, { 1.0f, 0.85f, 0.7f });
        return Textured(*n, x * 1.3f + 400.0f - f * 6.0f, y * 1.3f + 200.0f, { 0.6f, 0.8f, 1.0f });
    } });
    return scenes;
}

// 경계가 계단처럼 튀지 않게 픽셀마다 4점을 평균해 그린다.
std::vector<uint32_t> Render(const Scene& scene, float frame, int w, int h)
{
    std::vector<uint32_t> px((size_t)w * h);
    static const float kOffsets[4][2] = { { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
    concurrency::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; x++) {
            float r = 0, g = 0, b = 0;
            for (auto& o : kOffsets) {
                Rgb c = scene(frame, x + o[0], y + o[1], w, h);
                r += c.r;
                g += c.g;
                b += c.b;
            }
            auto byte = [](float v) { return (uint32_t)std::clamp(v * 255.0f / 4.0f + 0.5f, 0.0f, 255.0f); };
            px[(size_t)y * w + x] = 0xFF000000 | (byte(b) << 16) | (byte(g) << 8) | byte(r);
        }
    });
    return px;
}

// 24비트 BMP로 저장 (RGBA 입력). 결과 확인은 tools/test에서 PNG로 묶는다.
void SaveBmp(const std::wstring& path, const std::vector<uint32_t>& rgba, int w, int h)
{
    const int rowBytes = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    file.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    std::vector<uint8_t> row(rowBytes);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = rgba[(size_t)y * w + x];
            row[x * 3] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = c & 0xFF;
        }
        file.write(reinterpret_cast<const char*>(row.data()), rowBytes);
    }
}

struct Totals {
    double hold = 0, blend = 0, synth = 0, blendNear = 0, synthNear = 0, extrap = 0, estimateMs = 0, synthMs = 0;
    int n = 0;
};

const int kSamples = 4;       // 장면마다 시험하는 프레임 묶음 수
const double kInterval = 1.0 / 30.0;  // 테스트 영상의 프레임 간격 (가정)

}  // namespace

// 사각형 안의 PSNR (RGBA 두 장)
double RectPsnr(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int w, int x0, int y0, int x1, int y1)
{
    double sum = 0;
    long long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint32_t p = a[(size_t)y * w + x], q = b[(size_t)y * w + x];
            for (int k = 0; k < 24; k += 8) {
                const double d = (double)((p >> k) & 0xFF) - (double)((q >> k) & 0xFF);
                sum += d * d;
            }
            n += 3;
        }
    const double mse = sum / std::max(n, 1LL) / (255.0 * 255.0);
    return mse <= 1e-10 ? 100.0 : -10.0 * std::log10(mse);
}

// 5. 1.6.0 처리: 같은 프레임으로 껐을 때와 켰을 때를 비교한다 -----------------------------
void RunFeatureTests(FrameGen& fg, const std::vector<SceneInfo>& scenes, const std::vector<SceneInfo>& extra, const std::wstring& outDir)
{
    auto find = [&](const char* key) -> const SceneInfo* {
        for (auto& s : scenes)
            if (strcmp(s.key, key) == 0) return &s;
        for (auto& s : extra)
            if (strcmp(s.key, key) == 0) return &s;
        return nullptr;
    };
    const int W = 1280, H = 720;
    const FgParams defaults;
    std::ofstream csv(outDir + L"\\fg_features.csv");
    csv << "test,scene,gap,off_db,on_db,off_ui_db,on_ui_db\n";

    // 고정 UI: 이웃한 프레임 쌍 56개(30fps면 1.9초) 또는 160개(5.3초)로 고정 픽셀을 쌓은 뒤 만든다. 전체와 UI 부분의 PSNR.
    // 끔 / 켬(고정 픽셀) / 증거(고정 픽셀 + 고정 UI 증거, FG_FEATURE_UIBOX). 증거는 128쌍 넘게 쌓여야 효과가 있다.
    printf("\n[고정 UI] 고정 픽셀 보호 끔 / 켬 / 증거 (1280x720, dB)\n%-10s %4s %4s %8s %8s %8s %8s %8s %8s\n", "장면", "간격", "쌓기", "전체 끔",
           "전체 켬", "전체 증거", "UI 끔", "UI 켬", "UI 증거");
    struct Ui { const char* key; int x0, y0, x1, y1; };
    const Ui uis[] = { { "pan_ui", (int)(W * 0.04f), (int)(H * 0.82f), (int)(W * 0.34f), (int)(H * 0.95f) },
                       { "crosshair", W / 2 - 16, H / 2 - 16, W / 2 + 16, H / 2 + 16 },
                       { "rhythm", (int)(W * 0.75f), (int)(H * 0.06f), (int)(W * 0.95f), (int)(H * 0.14f) } };
    for (auto& ui : uis) {
        const SceneInfo* scene = find(ui.key);
        for (int warm : { 56, 160 })
        for (int gap : { 1, 2 }) {
            double whole[3] = {}, part[3] = {};
            for (int i = 0; i < kSamples; i++) {
                const float base = 200.0f + i * 7.0f;
                fg.TestClearStill();
                auto previous = Render(scene->shade, base - warm, W, H);
                for (int k = 0; k < warm; k++) {
                    auto next = Render(scene->shade, base - warm + 1 + k, W, H);
                    fg.TestLoadFrame(0, previous.data(), W, H);
                    fg.TestLoadFrame(1, next.data(), W, H);
                    fg.TestUpdateStill(0, 1);
                    previous = std::move(next);
                }
                auto f0 = Render(scene->shade, base, W, H);
                auto f1 = Render(scene->shade, base + gap, W, H);
                auto ref = Render(scene->shade, base + gap * 0.5f, W, H);
                fg.TestLoadFrame(0, f0.data(), W, H);
                fg.TestLoadFrame(1, f1.data(), W, H);
                fg.TestLoadFrame(2, ref.data(), W, H);
                const int modes[3] = { 0, FG_FEATURE_STILL, FG_FEATURE_STILL | FG_FEATURE_UIBOX };
                for (int mode = 0; mode < 3; mode++) {
                    fg.SetFeatures(modes[mode]);
                    whole[mode] += fg.TestSynthPsnr(0, 1, 2, 0.5f, defaults, false, kInterval * gap).psnr;
                    auto out = fg.TestReadOutput();
                    part[mode] += RectPsnr(out, ref, W, ui.x0, ui.y0, ui.x1, ui.y1);
                    if (i == 1 && warm == 160) {
                        const std::wstring stem = outDir + L"\\feat_" + std::wstring(ui.key, ui.key + strlen(ui.key)) + L"_gap" + std::to_wstring(gap);
                        const wchar_t* names[3] = { L"_off.bmp", L"_on.bmp", L"_uibox.bmp" };
                        SaveBmp(stem + names[mode], out, W, H);
                        if (mode == 0) SaveBmp(stem + L"_truth.bmp", ref, W, H);
                    }
                }
            }
            const double k = 1.0 / kSamples;
            printf("%-10s %4d %4d %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n", ui.key, gap, warm, whole[0] * k, whole[1] * k, whole[2] * k, part[0] * k,
                   part[1] * k, part[2] * k);
            fflush(stdout);
            csv << "still," << ui.key << "," << gap << "_" << warm << "," << whole[0] * k << "," << whole[1] * k << "," << part[0] * k << "," << part[1] * k
                << "," << whole[2] * k << "," << part[2] * k << "\n";
        }
    }
    fg.TestClearStill();

    // 장면 전환: 움직임 격자에서 믿을 수 없는 칸의 비율. 전환은 기준(0.45)을 넘고 다른 장면은 넘지 않아야 한다.
    printf("\n[장면 전환] 믿을 수 없는 칸의 비율 (기준 0.45 초과면 전환, 1280x720)\n");
    fg.SetFeatures(FG_FEATURE_ALL);
    double worst = 0;
    const char* worstKey = "";
    std::vector<const SceneInfo*> normal;
    for (auto& s : scenes) normal.push_back(&s);
    for (auto& s : extra)
        if (strcmp(s.key, "cut") != 0) normal.push_back(&s);
    for (const SceneInfo* scene : normal) {
        double sceneMax = 0;
        for (int i = 0; i < kSamples; i++) {
            const float base = 3.0f + i * 4.0f;
            for (int gap : { 1, 4 }) {
                auto f0 = Render(scene->shade, base, W, H);
                auto f1 = Render(scene->shade, base + gap, W, H);
                fg.TestLoadFrame(0, f0.data(), W, H);
                fg.TestLoadFrame(1, f1.data(), W, H);
                sceneMax = std::max(sceneMax, (double)fg.TestCutScore(0, 1, gap > 1 ? FrameGen::ForLongGap(defaults) : defaults));
            }
        }
        printf("  %-10s 최대 %.3f\n", scene->key, sceneMax);
        csv << "cut," << scene->key << ",1/4," << sceneMax << ",,,\n";
        if (sceneMax > worst) {
            worst = sceneMax;
            worstKey = scene->key;
        }
    }
    const SceneInfo* cut = find("cut");
    auto c0 = Render(cut->shade, 10.0f, W, H);
    auto c1 = Render(cut->shade, 11.0f, W, H);
    fg.TestLoadFrame(0, c0.data(), W, H);
    fg.TestLoadFrame(1, c1.data(), W, H);
    const double cutScore = fg.TestCutScore(0, 1, defaults);
    printf("  %-10s %.3f  (다른 장면 중 가장 높은 값: %s %.3f)  %s\n", "cut", cutScore, worstKey, worst,
           cutScore > 0.45 && worst <= 0.45 ? "통과" : "실패");
    csv << "cut,cut,1," << cutScore << ",,,\n";
    fflush(stdout);

    // 밝기 변화: 제자리 상관 보정 끔 / 켬. 다른 장면이 나빠지지 않는지도 본다 (4칸은 긴 간격 설정).
    // 판정 글자(FG_FEATURE_POP): 제자리 상관 기준을 0.9에서 0.6으로 낮춘 결과도 함께 낸다.
    printf("\n[밝기 변화] 제자리 상관 보정 끔 / 켬 / 낮춘 기준 (1280x720, dB)\n%-10s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n", "장면", "1칸 끔", "1칸 켬",
           "1칸 낮춤", "2칸 끔", "2칸 켬", "2칸 낮춤", "4칸 끔", "4칸 켬", "4칸 낮춤");
    for (const SceneInfo* scene : normal) {
        double r[3][3] = {};
        for (int i = 0; i < kSamples; i++) {
            const float base = 3.0f + i * 4.0f;
            const int gaps[3] = { 1, 2, 4 };
            for (int g = 0; g < 3; g++) {
                const int gap = gaps[g];
                auto f0 = Render(scene->shade, base, W, H);
                auto f1 = Render(scene->shade, base + gap, W, H);
                auto ref = Render(scene->shade, base + gap * 0.5f, W, H);
                fg.TestLoadFrame(0, f0.data(), W, H);
                fg.TestLoadFrame(1, f1.data(), W, H);
                fg.TestLoadFrame(2, ref.data(), W, H);
                const FgParams params = gap >= 4 ? FrameGen::ForLongGap(defaults) : defaults;
                const int modes[3] = { 0, FG_FEATURE_FADE, FG_FEATURE_FADE | FG_FEATURE_POP };
                for (int mode = 0; mode < 3; mode++) {
                    fg.SetFeatures(modes[mode]);
                    r[g][mode] += fg.TestSynthPsnr(0, 1, 2, 0.5f, params, false, kInterval * gap).psnr;
                    if (i == 1 && gap == 2 && strcmp(scene->key, "flash_text") == 0)
                        SaveBmp(outDir + L"\\feat_flash_text" + (mode == 2 ? L"_pop.bmp" : mode ? L"_on.bmp" : L"_off.bmp"), fg.TestReadOutput(), W, H);
                }
            }
        }
        const double k = 1.0 / kSamples;
        printf("%-10s %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n", scene->key, r[0][0] * k, r[0][1] * k, r[0][2] * k, r[1][0] * k, r[1][1] * k,
               r[1][2] * k, r[2][0] * k, r[2][1] * k, r[2][2] * k);
        fflush(stdout);
        for (int g = 0; g < 3; g++) csv << "fade," << scene->key << "," << (1 << g) << "," << r[g][0] * k << "," << r[g][1] * k << ",," << r[g][2] * k << "\n";
    }
}

int RunFrameGenSelfTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir)
{
    MakeDirectories(outDir);
    NoiseTexture noise;
    auto scenes = MakeScenes(noise);
    FrameGen fg(device, context);

    std::ofstream csv(outDir + L"\\fg_selftest.csv");
    csv << "size,scene,hold_db,blend_db,motion_db,near_blend_db,near_motion_db,extrapolate_db,estimate_ms,synth_ms\n";
    // 2칸: F0, F2로 F1을 만든다 (학습 채점과 같은 조건). 1칸: 실제 보간처럼 이웃한 두 프레임 사이의 정답과 비교한다.
    printf("%-10s %-10s %8s %9s %9s %9s %9s %8s %10s %9s\n", "크기", "장면", "멈춤", "블렌딩2칸", "움직임2칸", "블렌딩1칸", "움직임1칸", "외삽",
           "추정 ms", "합성 ms");

    const struct { int w, h; } sizes[] = { { 640, 360 }, { 1280, 720 }, { 1920, 1080 } };
    FgParams defaults;

    // 1. 기본 설정 점수 ---------------------------------------------------------
    for (auto& size : sizes) {
        for (auto& scene : scenes) {
            Totals total;
            fg.TestResetTimers();
            for (int i = 0; i < kSamples; i++) {
                float base = 3.0f + i * 4.0f;
                auto f0 = Render(scene.shade, base, size.w, size.h);
                auto f1 = Render(scene.shade, base + 1, size.w, size.h);
                auto f2 = Render(scene.shade, base + 2, size.w, size.h);
                fg.TestLoadFrame(0, f0.data(), size.w, size.h);
                fg.TestLoadFrame(1, f1.data(), size.w, size.h);
                fg.TestLoadFrame(2, f2.data(), size.w, size.h);

                total.hold += fg.TestHoldPsnr(0, 1);
                total.blend += fg.TestBlendPsnr(0, 2, 1, 0.5f);
                auto r = fg.TestSynthPsnr(0, 2, 1, 0.5f, defaults, false, kInterval * 2);
                total.synth += r.psnr;
                total.estimateMs += r.estimateMs;
                total.synthMs += r.synthMs;

                if (i == 2 && size.w == 1280) {
                    std::wstring name = outDir + L"\\" + std::wstring(scene.key, scene.key + strlen(scene.key));
                    SaveBmp(name + L"_truth.bmp", f1, size.w, size.h);
                    SaveBmp(name + L"_motion.bmp", fg.TestReadOutput(), size.w, size.h);
                    fg.TestBlendPsnr(0, 2, 1, 0.5f);
                    SaveBmp(name + L"_blend.bmp", fg.TestReadOutput(), size.w, size.h);
                    SaveBmp(name + L"_flow.bmp", fg.TestReadVisualize(0, 2, defaults, 0.5f), size.w, size.h);
                }

                // 실제 보간: 이웃한 F0, F1 사이의 한가운데 (정답은 계산으로 그릴 수 있다)
                auto middle = Render(scene.shade, base + 0.5f, size.w, size.h);
                fg.TestLoadFrame(2, middle.data(), size.w, size.h);
                total.blendNear += fg.TestBlendPsnr(0, 1, 2, 0.5f);
                total.synthNear += fg.TestSynthPsnr(0, 1, 2, 0.5f, defaults, false, kInterval).psnr;
                fg.TestLoadFrame(2, f2.data(), size.w, size.h);

                // 외삽: F0, F1로 F2를 예측한다 (t = 2는 시작 범위를 넘지만 등속 가정을 시험하기에 적당하다)
                total.extrap += fg.TestSynthPsnr(0, 1, 2, 2.0f, defaults, false, kInterval).psnr;
                total.n++;
            }
            double k = 1.0 / total.n;
            printf("%4dx%-5d %-10s %8.2f %9.2f %9.2f %9.2f %9.2f %8.2f %10.2f %9.2f\n", size.w, size.h, scene.key, total.hold * k, total.blend * k,
                   total.synth * k, total.blendNear * k, total.synthNear * k, total.extrap * k, total.estimateMs * k, total.synthMs * k);
            fflush(stdout);
            csv << size.w << "x" << size.h << "," << scene.key << "," << total.hold * k << "," << total.blend * k << "," << total.synth * k
                << "," << total.blendNear * k << "," << total.synthNear * k << "," << total.extrap * k << "," << total.estimateMs * k << "," << total.synthMs * k << "\n";
        }
    }

    // 2. RIFE: 블록 매칭과 같은 영상, 같은 조건으로 비교한다. 다른 시점(0.25)도 잰다 ---------
    // 신경망은 시점 0.5의 움직임만 구하므로 0.25는 근사다.
    std::ofstream rifeCsv(outDir + L"\\fg_rife.csv");
    rifeCsv << "size,quality,scene,block_half,rife_half,block_quarter,rife_quarter,infer_ms\n";
    printf("\n[RIFE] 이웃한 두 프레임 사이 (dB)\n%-10s %-5s %-10s %10s %9s %10s %9s %10s\n", "크기", "품질", "장면", "블록 t=.5", "RIFE t=.5",
           "블록 t=.25", "RIFE t=.25", "신경망 ms");
    bool rifeOk = true;
    const struct { int w, h, quality; } rifeRuns[] = { { 640, 360, 1 }, { 1280, 720, 0 }, { 1280, 720, 1 }, { 1280, 720, 2 }, { 1920, 1080, 1 } };
    for (auto& run : rifeRuns) {
        for (auto& scene : scenes) {
            double blockHalf = 0, rifeHalf = 0, blockQuarter = 0, rifeQuarter = 0, inferMs = 0;
            for (int i = 0; i < kSamples && rifeOk; i++) {
                float base = 3.0f + i * 4.0f;
                auto f0 = Render(scene.shade, base, run.w, run.h);
                auto f1 = Render(scene.shade, base + 1, run.w, run.h);
                auto half = Render(scene.shade, base + 0.5f, run.w, run.h);
                auto quarter = Render(scene.shade, base + 0.25f, run.w, run.h);
                fg.TestLoadFrame(0, f0.data(), run.w, run.h);
                fg.TestLoadFrame(1, f1.data(), run.w, run.h);
                fg.TestLoadFrame(2, half.data(), run.w, run.h);
                if (!fg.TestRifeRun(0, 1, run.quality)) {
                    printf("RIFE를 쓸 수 없어 건너뜁니다: %s\n", fg.TestRifeError().c_str());
                    rifeOk = false;
                    break;
                }
                blockHalf += fg.TestSynthPsnr(0, 1, 2, 0.5f, defaults, false, kInterval).psnr;
                auto r = fg.TestRifePsnr(0, 1, 2, 0.5f);
                rifeHalf += r.psnr;
                inferMs += r.inferMs;
                if (i == 2 && run.w == 1280 && run.quality == 1) {
                    std::wstring name = outDir + L"\\" + std::wstring(scene.key, scene.key + strlen(scene.key));
                    SaveBmp(name + L"_rife.bmp", fg.TestReadOutput(), run.w, run.h);
                }
                fg.TestLoadFrame(2, quarter.data(), run.w, run.h);
                blockQuarter += fg.TestSynthPsnr(0, 1, 2, 0.25f, defaults, false, kInterval).psnr;
                rifeQuarter += fg.TestRifePsnr(0, 1, 2, 0.25f).psnr;
            }
            if (!rifeOk) break;
            double k = 1.0 / kSamples;
            printf("%4dx%-5d %-5d %-10s %10.2f %9.2f %10.2f %9.2f %10.1f\n", run.w, run.h, run.quality, scene.key, blockHalf * k, rifeHalf * k,
                   blockQuarter * k, rifeQuarter * k, inferMs * k);
            fflush(stdout);
            rifeCsv << run.w << "x" << run.h << "," << run.quality << "," << scene.key << "," << blockHalf * k << "," << rifeHalf * k << ","
                    << blockQuarter * k << "," << rifeQuarter * k << "," << inferMs * k << "\n";
        }
        if (!rifeOk) break;
    }

    // 3. 게임별 학습: 앞 구간에서 배우고, 보지 않은 뒤 구간에서 처음 설정과 비교한다 ------
    printf("\n[학습] 장면별 %d회 채점 후, 보지 않은 프레임으로 비교 (640x360)\n", 120);
    printf("%-10s %9s %9s %9s %6s %-8s %s\n", "장면", "처음", "학습 후", "지도 사용", "채택", "유형", "학습된 설정");
    std::ofstream learn(outDir + L"\\fg_learning.csv");
    learn << "scene,default_db,learned_db,learned_map_db,adopted,genre,cell,radius,smooth,tol,history\n";
    const int w = 640, h = 360;

    for (auto& scene : scenes) {
        GameProfile profile;
        fg.SetProfile(&profile);

        for (int i = 0; i < 120; i++) {
            float base = 50.0f + i * 1.7f;
            auto f0 = Render(scene.shade, base, w, h);
            auto f1 = Render(scene.shade, base + 1, w, h);
            auto f2 = Render(scene.shade, base + 2, w, h);
            fg.TestLoadFrame(0, f0.data(), w, h);
            fg.TestLoadFrame(1, f1.data(), w, h);
            fg.TestLoadFrame(2, f2.data(), w, h);
            // 화면 지도 표본은 실제 프로그램처럼 이웃한 두 프레임의 움직임에서 뽑는다.
            fg.TestSynthPsnr(0, 1, 2, 0.5f, profile.interp.params, false, kInterval);
            fg.TestAddFlowSample(profile.interp.params, kInterval);
            fg.TestUpdateMapTexture();
            float current = fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.interp.params, false, kInterval * 2).psnr;
            float candidate = fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.interp.Candidate(), false, kInterval * 2).psnr;
            float blend = fg.TestBlendPsnr(0, 2, 1, 0.5f);
            profile.interp.AddEvaluation(current, candidate, blend, profile.mapSamples);
        }
        profile.UpdateGenre();
        fg.TestUpdateMapTexture();

        double before = 0, after = 0, withMap = 0;
        const int kHeldOut = 16;
        for (int i = 0; i < kHeldOut; i++) {
            float base = 600.0f + i * 5.3f;  // 학습에 쓰지 않은 구간
            auto f0 = Render(scene.shade, base, w, h);
            auto f1 = Render(scene.shade, base + 1, w, h);
            auto f2 = Render(scene.shade, base + 2, w, h);
            fg.TestLoadFrame(0, f0.data(), w, h);
            fg.TestLoadFrame(1, f1.data(), w, h);
            fg.TestLoadFrame(2, f2.data(), w, h);
            before += fg.TestSynthPsnr(0, 2, 1, 0.5f, defaults, false, kInterval * 2).psnr;
            after += fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.interp.params, false, kInterval * 2).psnr;
            withMap += fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.interp.params, true, kInterval * 2).psnr;
        }
        before /= kHeldOut;
        after /= kHeldOut;
        withMap /= kHeldOut;
        printf("%-10s %9.2f %9.2f %9.2f %6d %-8s 칸 %d, 범위 %d, 벌점 %.4f, 허용 %.2f\n", scene.key, before, after, withMap, profile.interp.adoptedCount,
               kGenreKeys[profile.genre], profile.interp.params.cellPx, profile.interp.params.coarseRadius, profile.interp.params.smoothness, profile.interp.params.blendTol);
        fflush(stdout);
        learn << scene.key << "," << before << "," << after << "," << withMap << "," << profile.interp.adoptedCount << "," << kGenreKeys[profile.genre]
              << "," << profile.interp.params.cellPx << "," << profile.interp.params.coarseRadius << "," << profile.interp.params.smoothness << ","
              << profile.interp.params.blendTol << ",";
        for (size_t i = 0; i < profile.interp.history.size(); i++) learn << (i ? ";" : "") << profile.interp.history[i];
        learn << "\n";
        fg.SetProfile(nullptr);
    }

    // 4. 긴 간격: 게임이 멈칫해 4칸 만에 다음 프레임이 온 경우. 기본 설정과 긴 간격 설정을 비교한다 ------
    printf("\n[긴 간격] F0, F4로 F2를 만든다 (1280x720, dB)\n%-10s %9s %9s %12s\n", "장면", "블렌딩", "기본", "긴 간격 설정");
    std::ofstream longCsv(outDir + L"\\fg_longgap.csv");
    longCsv << "scene,blend_db,default_db,longgap_db\n";
    const FgParams longGap = FrameGen::ForLongGap(defaults);
    for (auto& scene : scenes) {
        double blend = 0, before = 0, after = 0;
        for (int i = 0; i < kSamples; i++) {
            float base = 3.0f + i * 6.0f;
            auto f0 = Render(scene.shade, base, 1280, 720);
            auto f2 = Render(scene.shade, base + 2, 1280, 720);
            auto f4 = Render(scene.shade, base + 4, 1280, 720);
            fg.TestLoadFrame(0, f0.data(), 1280, 720);
            fg.TestLoadFrame(1, f2.data(), 1280, 720);
            fg.TestLoadFrame(2, f4.data(), 1280, 720);
            blend += fg.TestBlendPsnr(0, 2, 1, 0.5f);
            before += fg.TestSynthPsnr(0, 2, 1, 0.5f, defaults, false, kInterval * 4).psnr;
            after += fg.TestSynthPsnr(0, 2, 1, 0.5f, longGap, false, kInterval * 4).psnr;
        }
        const double k = 1.0 / kSamples;
        printf("%-10s %9.2f %9.2f %12.2f\n", scene.key, blend * k, before * k, after * k);
        fflush(stdout);
        longCsv << scene.key << "," << blend * k << "," << before * k << "," << after * k << "\n";
    }

    RunFeatureTests(fg, scenes, MakeExtraScenes(noise), outDir);
    fg.SetFeatures(FG_FEATURE_ALL);
    return 0;
}

int RunFrameGenRegionTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir)
{
    MakeDirectories(outDir);
    NoiseTexture noise;
    auto scenes = MakeScenes(noise);
    const int w = 960, h = 540;
    const int rx = 240, ry = 60, rw = 400, rh = 420;  // 일부러 화면 가운데의 어중간한 위치
    auto makeTexture = [&](const std::vector<uint32_t>& rgba, int tw, int th, int ox, int oy) {
        std::vector<uint32_t> bgra((size_t)tw * th);
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                uint32_t c = rgba[(size_t)(y + oy) * w + x + ox];
                bgra[(size_t)y * tw + x] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
            }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = tw;
        td.Height = th;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { bgra.data(), (UINT)tw * 4, 0 };
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(device->CreateTexture2D(&td, &data, tex.put()));
        return tex;
    };

    int failures = 0;
    printf("[영역 시험] %dx%d 화면, 영역 (%d, %d) %dx%d\n", w, h, rx, ry, rw, rh);
    for (const char* key : { "pan", "rhythm", "sprites" }) {
        const SceneInfo* scene = nullptr;
        for (auto& s : scenes)
            if (strcmp(s.key, key) == 0) scene = &s;
        if (!scene) continue;
        FrameGen withRegion(device, context), cropped(device, context);
        FgOptions region, plain;
        region.learn = plain.learn = false;
        region.refreshHz = plain.refreshHz = 144;
        region.regionX = rx;
        region.regionY = ry;
        region.regionW = rw;
        region.regionH = rh;
        std::vector<std::vector<uint32_t>> frames;
        for (int i = 0; i < 4; i++) frames.push_back(Render(scene->shade, 10.0f + i, w, h));
        int checked = 0, generated = 0, insideBad = 0, outsideBad = 0, heldBad = 0;
        for (int i = 0; i < 4; i++) {
            auto full = makeTexture(frames[i], w, h, 0, 0);
            auto part = makeTexture(frames[i], rw, rh, rx, ry);
            const double time = i * kInterval;
            withRegion.PushFrame(full.get(), w, h, time, region);
            cropped.PushFrame(part.get(), rw, rh, time, plain);
            for (int k = 0; k < 6; k++) {
                const double now = time + kInterval * (k / 6.0);
                ID3D11ShaderResourceView* a = withRegion.Render(now, region);
                ID3D11ShaderResourceView* b = cropped.Render(now, plain);
                if (!a || !b) continue;
                UINT aw, ah, bw, bh;
                auto outA = withRegion.TestReadSrv(a, aw, ah);
                auto outB = cropped.TestReadSrv(b, bw, bh);
                checked++;
                const float t = (float)withRegion.Stats().displayT;
                if (t > 0.005f && t < 0.995f) generated++;
                if (aw != (UINT)w || ah != (UINT)h || bw != (UINT)rw || bh != (UINT)rh) {
                    insideBad++;
                    continue;
                }
                // 영역 안: 그 부분만 넣은 프레임 생성과 같아야 한다.
                for (int y = 0; y < rh; y++)
                    for (int x = 0; x < rw; x++)
                        if ((outA[(size_t)(y + ry) * w + x + rx] & 0xFFFFFF) != (outB[(size_t)y * rw + x] & 0xFFFFFF)) insideBad++;
                // 영역 밖: 넣은 게임 프레임 중 하나와 똑같아야 한다.
                bool anyMatch = false;
                for (int j = 0; j <= i && !anyMatch; j++) {
                    bool same = true;
                    for (int y = 0; y < h && same; y += 3)
                        for (int x = 0; x < w && same; x += 3) {
                            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) continue;
                            if ((outA[(size_t)y * w + x] & 0xFFFFFF) != (frames[j][(size_t)y * w + x] & 0xFFFFFF)) same = false;
                        }
                    anyMatch = same;
                }
                if (!anyMatch) outsideBad++;
                // 비교 모드에 쓰는 프레임도 화면 전체 크기여야 한다.
                UINT hw = 0, hh = 0;
                if (auto held = withRegion.HeldFrame()) withRegion.TestReadSrv(held, hw, hh);
                if (hw != (UINT)w || hh != (UINT)h) heldBad++;
                if (i == 3 && k == 3) SaveFrameBmp(outDir + L"\\region_" + std::wstring(key, key + strlen(key)) + L".bmp", outA, w, h);
            }
        }
        const bool ok = checked > 0 && generated > 0 && insideBad == 0 && outsideBad == 0 && heldBad == 0;
        printf("%-10s 확인 %2d장 (만든 장면 %2d) · 영역 안 다른 픽셀 %d · 영역 밖 어긋난 장 %d · 비교 영상 크기 오류 %d  %s\n", key, checked, generated,
               insideBad, outsideBad, heldBad, ok ? "통과" : "실패");
        if (!ok) failures++;
    }
    return failures == 0 ? 0 : 1;
}

void SaveFrameBmp(const std::wstring& path, const std::vector<uint32_t>& rgba, int w, int h)
{
    SaveBmp(path, rgba, w, h);
}

namespace {

// SaveBmp가 쓴 24비트 BMP를 RGBA로 읽는다.
bool LoadBmp(const std::wstring& path, std::vector<uint32_t>& rgba, int& w, int& h)
{
    std::ifstream file(path, std::ios::binary);
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    file.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    file.read(reinterpret_cast<char*>(&ih), sizeof(ih));
    if (!file || fh.bfType != 0x4D42 || ih.biBitCount != 24 || ih.biHeight <= 0) return false;
    w = ih.biWidth;
    h = ih.biHeight;
    const int rowBytes = (w * 3 + 3) & ~3;
    file.seekg(fh.bfOffBits);
    rgba.assign((size_t)w * h, 0);
    std::vector<uint8_t> row(rowBytes);
    for (int y = h - 1; y >= 0; y--) {
        file.read(reinterpret_cast<char*>(row.data()), rowBytes);
        for (int x = 0; x < w; x++)
            rgba[(size_t)y * w + x] = 0xFF000000u | row[x * 3 + 2] | (row[x * 3 + 1] << 8) | (row[x * 3] << 16);
    }
    return (bool)file;
}

}  // namespace

int RunFrameGenGapTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                       const std::wstring& profilePath)
{
    // 슬로모션 덤프의 진짜 프레임만 쓴다. 이웃한 진짜 프레임의 간격은 real_times.txt에서 읽는다 (없으면 60fps로 본다).
    std::vector<std::vector<uint32_t>> frames;
    int w = 0, h = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dumpDir + L"\\frame_*_real.bmp").c_str(), &fd);
    std::vector<std::wstring> names;
    if (find != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName);
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(names.begin(), names.end());
    for (auto& name : names) {
        std::vector<uint32_t> rgba;
        int fw, fh;
        if (!LoadBmp(dumpDir + L"\\" + name, rgba, fw, fh)) continue;
        if (!frames.empty() && frames.back() == rgba) continue;  // 녹화 첫 장 반복 등 같은 프레임은 건너뛴다
        w = fw;
        h = fh;
        frames.push_back(std::move(rgba));
    }
    if (frames.size() < 12) {
        printf("진짜 프레임이 부족합니다: %zu장\n", frames.size());
        return 1;
    }
    MakeDirectories(outDir);
    FrameGen fg(device, context);
    FgParams defaults;
    // 이웃한 게임 프레임의 간격. 덤프에 캡처 시각이 있으면 그 중앙값을 쓰고, 없으면 60fps 캡처로 본다.
    // 게임 fps를 제한한 채 녹화하면 이웃한 프레임도 33ms처럼 벌어져 있어, "2칸"이 더 느린 게임에 해당한다.
    double kStep = 1.0 / 60.0;
    {
        std::ifstream timesFile(dumpDir + L"\\real_times.txt");
        std::vector<double> times, gaps;
        for (double v; timesFile >> v;) times.push_back(v);
        for (size_t i = 1; i < times.size(); i++) gaps.push_back(times[i] - times[i - 1]);
        if (gaps.size() >= 4) {
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            kStep = gaps[gaps.size() / 2];
        }
    }
    printf("이웃한 게임 프레임 간격: %.1f ms\n", kStep * 1000);

    // 진짜 프레임 k와 k+gap으로 가운데 프레임을 만들고 실제 프레임과 비교한다.
    // 긴 간격 설정: 실시간처럼 1칸을 보통 간격으로 보고, 그보다 훨씬 긴 쌍에만 넓은 칸과 낮은 벌점을 쓴다.
    printf("진짜 프레임 %zu장 (%dx%d)\n%-6s %-6s %8s %9s %9s %10s %9s\n", frames.size(), w, h, "간격", "시점", "멈춤", "블렌딩", "블록",
           "긴 간격 설정", "RIFE");
    std::ofstream csv(outDir + L"\\fg_gaptest.csv");
    csv << "gap,t,samples,hold_db,blend_db,block_db,block_longgap_db,rife_db,frame_ms,block_estimate_ms,block_synth_ms\n";
    bool rifeOk = true;
    const struct { int gap, part; } cases[] = { { 2, 1 }, { 3, 1 }, { 4, 2 }, { 4, 1 }, { 6, 3 }, { 8, 4 } };
    for (auto& c : cases) {
        const float t = (float)c.part / c.gap;
        const FgParams longGap = FrameGen::IsLongGap(kStep * c.gap, kStep) ? FrameGen::ForLongGap(defaults) : defaults;
        double hold = 0, blend = 0, block = 0, blockLong = 0, rife = 0, estMs = 0, synMs = 0;  // 블록 매칭의 GPU 시간도 남긴다
        int n = 0, rifeN = 0;
        const int sampleAt = (int)frames.size() / 2;
        for (int k = 0; k + c.gap < (int)frames.size(); k += 2) {
            fg.TestLoadFrame(1, frames[k].data(), w, h);
            fg.TestLoadFrame(2, frames[k + c.gap].data(), w, h);
            fg.TestLoadFrame(3, frames[k + c.part].data(), w, h);
            const bool save = c.gap == 4 && c.part == 2 && k >= sampleAt && k < sampleAt + 2;
            std::wstring name = outDir + L"\\gap" + std::to_wstring(c.gap) + L"_";

            hold += fg.TestHoldPsnr(1, 3);
            blend += fg.TestBlendPsnr(1, 2, 3, t);
            if (save) SaveBmp(name + L"blend.bmp", fg.TestReadOutput(), w, h);
            const auto blockResult = fg.TestSynthPsnr(1, 2, 3, t, defaults, false, kStep * c.gap);
            block += blockResult.psnr;
            estMs += blockResult.estimateMs;
            synMs += blockResult.synthMs;
            if (save) {
                SaveBmp(name + L"block.bmp", fg.TestReadOutput(), w, h);
                SaveBmp(name + L"flow.bmp", fg.TestReadVisualize(1, 2, defaults, t), w, h);  // 움직임 시각화 (README 그림용)
            }
            blockLong += fg.TestSynthPsnr(1, 2, 3, t, longGap, false, kStep * c.gap).psnr;
            if (save) SaveBmp(name + L"block_longgap.bmp", fg.TestReadOutput(), w, h);
            if (rifeOk) {
                if (fg.TestRifeRun(1, 2, 1)) {
                    rife += fg.TestRifePsnr(1, 2, 3, t).psnr;
                    rifeN++;
                    if (save) SaveBmp(name + L"rife.bmp", fg.TestReadOutput(), w, h);
                } else {
                    printf("RIFE를 쓸 수 없어 건너뜁니다: %s\n", fg.TestRifeError().c_str());
                    rifeOk = false;
                }
            }
            if (save) {
                SaveBmp(name + L"a.bmp", frames[k], w, h);
                SaveBmp(name + L"truth.bmp", frames[k + c.part], w, h);
                SaveBmp(name + L"b.bmp", frames[k + c.gap], w, h);
            }
            n++;
        }
        const double k = 1.0 / n;
        const double rk = rifeN ? 1.0 / rifeN : 0;
        printf("%-6d %-6.2f %8.2f %9.2f %9.2f %10.2f %9.2f  (블록 매칭 GPU: 움직임 찾기 %.2f, 합성 %.2f ms)\n", c.gap, t, hold * k, blend * k,
               block * k, blockLong * k, rife * rk, estMs * k, synMs * k);
        fflush(stdout);
        csv << c.gap << "," << t << "," << n << "," << hold * k << "," << blend * k << "," << block * k << "," << blockLong * k << ","
            << rife * rk << "," << kStep * 1000 << "," << estMs * k << "," << synMs * k << "\n";
    }
    // 연속 재생: 진짜 프레임을 한 장씩 건너뛰어(60fps 녹화라면 30fps) 실제 프로그램처럼 차례로 넣고, 건너뛴 장을 만들어 비교한다.
    // 고정 픽셀 보호는 지난 쌍들로 쌓이므로 차례대로 넣어야 효과가 보인다. 1.6.0 처리(전부 켬)와 이전 처리(전부 끔)를 같은 쌍에서 비교한다.
    // "고정 픽셀"은 세 장(앞, 정답, 뒤)에서 값이 같은 픽셀이다. 깨짐은 그중 만든 장이 정답과 8/255 넘게 다른 비율.
    {
        printf("\n[연속 재생] 한 장씩 건너뛴 순서로 넣고 건너뛴 장과 비교 (보호가 쌓이기 전 48쌍은 따로)\n%-8s %6s %9s %9s %12s %12s %9s %9s\n", "구간",
               "쌍", "전체 이전", "전체 새", "고정 이전", "고정 새", "깨짐 이전", "깨짐 새");
        std::ofstream seqCsv(outDir + L"\\fg_gaptest_sequence.csv");
        seqCsv << "pair,k,old_db,new_db,static_px,old_broken,new_broken,cut_score\n";
        const int savedFeatures = fg.Features();
        fg.TestClearStill();
        struct Sum { double oldDb = 0, newDb = 0, oldStatic = 0, newStatic = 0; long long oldBroken = 0, newBroken = 0, px = 0; int n = 0, nStatic = 0; } sums[2];
        float cutMax = 0;
        int pair = 0;
        auto staticStats = [&](const std::vector<uint32_t>& out, const std::vector<uint32_t>& ref, const std::vector<uint8_t>& mask, double& mse,
                               long long& broken, long long& count) {
            mse = 0;
            broken = count = 0;
            for (size_t i = 0; i < out.size(); i++) {
                if (!mask[i]) continue;
                int worst = 0;
                for (int c = 0; c < 3; c++) {
                    const int d = (int)((out[i] >> (c * 8)) & 255) - (int)((ref[i] >> (c * 8)) & 255);
                    mse += d * d / 3.0;
                    worst = std::max(worst, std::abs(d));
                }
                broken += worst > 8;
                count++;
            }
            mse = count ? mse / count : 0;
        };
        for (int k = 0; k + 2 < (int)frames.size(); k += 2, pair++) {
            fg.TestLoadFrame(0, frames[k].data(), w, h);
            fg.TestLoadFrame(1, frames[k + 2].data(), w, h);
            fg.TestLoadFrame(2, frames[k + 1].data(), w, h);
            fg.SetFeatures(FG_FEATURE_ALL);
            const float cut = fg.TestCutScore(0, 1, defaults);
            cutMax = std::max(cutMax, cut);
            std::vector<uint8_t> mask(frames[k].size());
            for (size_t i = 0; i < mask.size(); i++)
                mask[i] = ((frames[k][i] ^ frames[k + 1][i]) & 0xFFFFFF) == 0 && ((frames[k + 2][i] ^ frames[k + 1][i]) & 0xFFFFFF) == 0;
            double db[2], mse[2];
            long long broken[2], count = 0;
            for (int mode = 0; mode < 2; mode++) {
                fg.SetFeatures(mode ? FG_FEATURE_ALL : 0);
                db[mode] = fg.TestSynthPsnr(0, 1, 2, 0.5f, defaults, false, kStep * 2).psnr;
                auto out = fg.TestReadOutput();
                staticStats(out, frames[k + 1], mask, mse[mode], broken[mode], count);
                if (pair == 60) {
                    SaveBmp(outDir + (mode ? L"\\seq_new.bmp" : L"\\seq_old.bmp"), out, w, h);
                    if (mode) SaveBmp(outDir + L"\\seq_truth.bmp", frames[k + 1], w, h);
                }
            }
            if (pair == 60) {
                // 이 쌍을 만들 때 쓴 고정 픽셀 상태를 정답 위에 칠한다 (CSCompose의 StillKeep과 같은 식).
                // 하늘색: 둘레도 멈춰 있어 보호, 분홍: 둘레가 계속 움직여 보호, 노랑: 오래 같았지만 둘레 조건에 걸려 보호하지 않음.
                // 이번 쌍에서 앞뒤가 4/255 넘게 다르면 보호하지 않으므로 칠하지 않는다.
                const auto still = fg.TestReadStill();
                std::vector<uint32_t> vis(frames[k + 1].size());
                for (size_t i = 0; i < vis.size(); i++) {
                    const uint32_t c = frames[k + 1][i];
                    float rgb[3] = { (c & 255) * 0.35f, ((c >> 8) & 255) * 0.35f, ((c >> 16) & 255) * 0.35f };
                    int diff = 0;
                    for (int ch = 0; ch < 3; ch++)
                        diff = std::max(diff, std::abs((int)((frames[k][i] >> (ch * 8)) & 255) - (int)((frames[k + 2][i] >> (ch * 8)) & 255)));
                    const float count = (float)(still[i] & 0xFF), moving = (float)((still[i] >> 8) & 0xFFFF) / 65535.0f;
                    const float old = std::clamp((count - 40.0f) / 8.0f, 0.0f, 1.0f);
                    const float quiet = std::clamp((0.05f - moving) / 0.03f, 0.0f, 1.0f), busy = std::clamp((moving - 0.7f) / 0.1f, 0.0f, 1.0f);
                    float color[3] = { 0, 0, 0 }, amount = 0;
                    if (diff < 4 && old > 0) {
                        if (quiet >= busy && quiet > 0) { color[0] = 80; color[1] = 200; color[2] = 255; amount = old * quiet; }
                        else if (busy > 0) { color[0] = 255; color[1] = 90; color[2] = 200; amount = old * busy; }
                        else { color[0] = 255; color[1] = 220; color[2] = 60; amount = old * 0.6f; }
                    }
                    for (int ch = 0; ch < 3; ch++) rgb[ch] = rgb[ch] * (1 - amount) + color[ch] * amount;
                    vis[i] = 0xFF000000u | (uint32_t)rgb[0] | ((uint32_t)rgb[1] << 8) | ((uint32_t)rgb[2] << 16);
                }
                SaveBmp(outDir + L"\\seq_still.bmp", vis, w, h);
            }
            // 보호 상태는 이 쌍을 만든 뒤에 이 쌍으로 갱신한다 (실제 프로그램에서도 다음 쌍부터 쓰인다).
            fg.SetFeatures(FG_FEATURE_ALL);
            fg.TestUpdateStill(0, 1);
            Sum& sum = sums[pair >= 48];
            sum.oldDb += db[0];
            sum.newDb += db[1];
            if (count > 0) {
                sum.oldStatic += mse[0] > 0 ? 10 * std::log10(255.0 * 255.0 / mse[0]) : 99;
                sum.newStatic += mse[1] > 0 ? 10 * std::log10(255.0 * 255.0 / mse[1]) : 99;
                sum.nStatic++;
            }
            sum.oldBroken += broken[0];
            sum.newBroken += broken[1];
            sum.px += count;
            sum.n++;
            seqCsv << pair << "," << k << "," << db[0] << "," << db[1] << "," << count << "," << broken[0] << "," << broken[1] << "," << cut << "\n";
        }
        const char* names[2] = { "앞 48쌍", "그 뒤" };
        for (int i = 0; i < 2; i++) {
            const Sum& sm = sums[i];
            if (!sm.n) continue;
            const double kn = 1.0 / sm.n, ks = sm.nStatic ? 1.0 / sm.nStatic : 0, kp = sm.px ? 100.0 / sm.px : 0;
            printf("%-8s %6d %9.2f %9.2f %12.2f %12.2f %8.2f%% %8.2f%%\n", names[i], sm.n, sm.oldDb * kn, sm.newDb * kn, sm.oldStatic * ks,
                   sm.newStatic * ks, sm.oldBroken * kp, sm.newBroken * kp);
        }
        printf("장면 전환 점수 최대 %.3f (전환으로 보는 기준 0.45)\n", cutMax);
        fflush(stdout);
        fg.SetFeatures(savedFeatures);
        fg.TestClearStill();
    }
    // 학습 재현: 앞 절반의 프레임으로 실제 프로그램과 같은 순서로 학습하고, 보지 않은 뒤 절반에서 기본값과 비교한다.
    // 다른 화면이 섞인 경우: 게임 프레임과 계산으로 만든 장면을 24회씩 번갈아 채점한다 (메뉴, 결과 화면 대신).
    NoiseTexture noise;
    auto scenes = MakeScenes(noise);
    std::ofstream learnCsv(outDir + L"\\fg_gaptest_learning.csv");
    learnCsv << "mixed_with,default_db,learned_db,adopted,cell,radius,smooth,tol,map\n";
    printf("\n[학습 재현] 앞 절반으로 학습, 보지 않은 뒤 절반의 2칸 비교 (dB)\n%-12s %8s %8s %6s %s\n", "섞은 장면", "기본", "학습", "채택", "학습된 값");
    for (int mix = -1; mix < (int)scenes.size(); mix++) {
        GameProfile fresh;
        fg.SetProfile(&fresh);
        const int half = (int)frames.size() / 2;
        const int kIterations = 360;
        for (int i = 0; i < kIterations; i++) {
            const int k = (i * 7) % (half - 2);
            std::vector<uint32_t> s0, s1, s2;
            if (mix >= 0 && (i / 24) % 2 == 1) {
                float base = 10.0f + i * 1.3f;
                s0 = Render(scenes[mix].shade, base, w, h);
                s1 = Render(scenes[mix].shade, base + 1, w, h);
                s2 = Render(scenes[mix].shade, base + 2, w, h);
            }
            fg.TestLoadFrame(0, s0.empty() ? frames[k].data() : s0.data(), w, h);
            fg.TestLoadFrame(1, s1.empty() ? frames[k + 1].data() : s1.data(), w, h);
            fg.TestLoadFrame(2, s2.empty() ? frames[k + 2].data() : s2.data(), w, h);
            if (i % 4 == 0) {
                fg.TestSynthPsnr(0, 1, 2, 0.5f, fresh.interp.params, false, kStep);
                fg.TestAddFlowSample(fresh.interp.params, kStep);
                fg.TestUpdateMapTexture();
            }
            float current = fg.TestSynthPsnr(0, 2, 1, 0.5f, fresh.interp.params, false, kStep * 2).psnr;
            float candidate = fg.TestSynthPsnr(0, 2, 1, 0.5f, fresh.interp.Candidate(), false, kStep * 2).psnr;
            fresh.interp.AddEvaluation(current, candidate, fg.TestBlendPsnr(0, 2, 1, 0.5f), fresh.mapSamples);
        }
        fg.TestUpdateMapTexture();
        double before = 0, after = 0;
        int n = 0;
        for (int k = half; k + 2 < (int)frames.size(); k++) {
            fg.TestLoadFrame(0, frames[k].data(), w, h);
            fg.TestLoadFrame(1, frames[k + 1].data(), w, h);
            fg.TestLoadFrame(2, frames[k + 2].data(), w, h);
            before += fg.TestSynthPsnr(0, 2, 1, 0.5f, defaults, false, kStep * 2).psnr;
            after += fg.TestSynthPsnr(0, 2, 1, 0.5f, fresh.interp.params, false, kStep * 2).psnr;
            n++;
        }
        const char* mixName = mix < 0 ? "없음" : scenes[mix].key;
        printf("%-12s %8.2f %8.2f %6d 칸 %d, 범위 %d, 벌점 %.4f, 허용 %.2f, 지도 %s\n", mixName, before / n, after / n, fresh.interp.adoptedCount,
               fresh.interp.params.cellPx, fresh.interp.params.coarseRadius, fresh.interp.params.smoothness, fresh.interp.params.blendTol, fresh.interp.params.useMap ? "사용" : "안 씀");
        fflush(stdout);
        learnCsv << (mix < 0 ? "none" : scenes[mix].key) << "," << before / n << "," << after / n << "," << fresh.interp.adoptedCount << ","
                 << fresh.interp.params.cellPx << "," << fresh.interp.params.coarseRadius << "," << fresh.interp.params.smoothness << "," << fresh.interp.params.blendTol << ","
                 << fresh.interp.params.useMap << "\n";
        fg.SetProfile(nullptr);
    }

    // 외삽 학습 재현: 게임 프레임 k, k+2로 k+4를 예측한다 (30fps 게임을 외삽하는 것과 같은 간격).
    // 기준은 프레임 생성이 없을 때와 같은 "앞 프레임 그대로"다. 학습 방식은 실시간의 외삽 채점과 같다.
    std::ofstream extrapCsv(outDir + L"\\fg_gaptest_extrap_learning.csv");
    extrapCsv << "mixed_with,hold_db,default_db,learned_db,adopted,cell,radius,smooth,tol,map\n";
    printf("\n[외삽 학습 재현] 앞 절반으로 학습, 보지 않은 뒤 절반에서 두 장으로 다음 장 예측 (dB)\n%-12s %8s %8s %8s %6s %s\n", "섞은 장면",
           "멈춤", "기본", "학습", "채택", "학습된 값");
    for (int mix = -1; mix < (int)scenes.size(); mix++) {
        GameProfile fresh;
        fg.SetProfile(&fresh);
        const int half = (int)frames.size() / 2;
        const int kIterations = 360;
        for (int i = 0; i < kIterations; i++) {
            const int k = (i * 7) % (half - 4);
            std::vector<uint32_t> s0, s1, s2;
            if (mix >= 0 && (i / 24) % 2 == 1) {
                float base = 10.0f + i * 1.3f;
                s0 = Render(scenes[mix].shade, base, w, h);
                s1 = Render(scenes[mix].shade, base + 1, w, h);
                s2 = Render(scenes[mix].shade, base + 2, w, h);
            }
            fg.TestLoadFrame(0, s0.empty() ? frames[k].data() : s0.data(), w, h);
            fg.TestLoadFrame(1, s1.empty() ? frames[k + 2].data() : s1.data(), w, h);
            fg.TestLoadFrame(2, s2.empty() ? frames[k + 4].data() : s2.data(), w, h);
            const FgParams current = fresh.extrap.params;
            const FgParams candidate = fresh.extrap.Candidate();
            float currentDb = fg.TestSynthPsnr(0, 1, 2, 2.0f, current, false, kStep * 2).psnr;
            if (i % 4 == 0) {
                fg.TestAddFlowSample(current, kStep * 2);
                fg.TestUpdateMapTexture();
            }
            float candidateDb = fg.TestSynthPsnr(0, 1, 2, 2.0f, candidate, false, kStep * 2).psnr;
            fresh.extrap.AddEvaluation(currentDb, candidateDb, fg.TestHoldPsnr(1, 2), fresh.mapSamples);
        }
        fg.TestUpdateMapTexture();
        double hold = 0, before = 0, after = 0;
        int n = 0;
        for (int k = half; k + 4 < (int)frames.size(); k++) {
            fg.TestLoadFrame(0, frames[k].data(), w, h);
            fg.TestLoadFrame(1, frames[k + 2].data(), w, h);
            fg.TestLoadFrame(2, frames[k + 4].data(), w, h);
            hold += fg.TestHoldPsnr(1, 2);
            before += fg.TestSynthPsnr(0, 1, 2, 2.0f, defaults, false, kStep * 2).psnr;
            after += fg.TestSynthPsnr(0, 1, 2, 2.0f, fresh.extrap.params, false, kStep * 2).psnr;
            n++;
        }
        const FgParams& p = fresh.extrap.params;
        const char* mixName = mix < 0 ? "없음" : scenes[mix].key;
        printf("%-12s %8.2f %8.2f %8.2f %6d 칸 %d, 범위 %d, 벌점 %.4f, 허용 %.2f, 지도 %s\n", mixName, hold / n, before / n, after / n,
               fresh.extrap.adoptedCount, p.cellPx, p.coarseRadius, p.smoothness, p.blendTol, p.useMap ? "사용" : "안 씀");
        fflush(stdout);
        extrapCsv << (mix < 0 ? "none" : scenes[mix].key) << "," << hold / n << "," << before / n << "," << after / n << "," << fresh.extrap.adoptedCount
                  << "," << p.cellPx << "," << p.coarseRadius << "," << p.smoothness << "," << p.blendTol << "," << p.useMap << "\n";
        fg.SetProfile(nullptr);
    }

    if (profilePath.empty()) return 0;

    // 게임별 학습: 같은 프레임으로 기본값과 학습된 설정, 화면 지도의 효과를 따로 잰다.
    // 실시간과 같이 각 설정에 긴 간격 처리를 덧붙인다.
    GameProfile profile;
    if (!profile.Load(profilePath)) {
        printf("프로필을 읽지 못했습니다\n");
        return 1;
    }
    fg.SetProfile(&profile);
    fg.TestUpdateMapTexture();
    const bool mapReady = profile.mapSamples >= 20;
    FgParams learned = profile.interp.params;
    learned.useMap = false;
    printf("\n[학습] 칸 %d, 범위 %d, 벌점 %.4f, 허용 %.2f, 지도 %s (표본 %d, 채점 %d회, 채택 %d회)\n", learned.cellPx, learned.coarseRadius,
           learned.smoothness, learned.blendTol, profile.interp.params.useMap ? "사용" : "안 씀", profile.mapSamples, profile.interp.evalCount,
           profile.interp.adoptedCount);
    printf("%-6s %-6s %9s %10s %12s %12s\n", "간격", "시점", "기본", "기본+지도", "학습(지도X)", "학습+지도");
    std::ofstream profileCsv(outDir + L"\\fg_gaptest_profile.csv");
    profileCsv << "gap,t,default_db,default_map_db,learned_db,learned_map_db\n";
    for (auto& c : cases) {
        const float t = (float)c.part / c.gap;
        const bool longGap = FrameGen::IsLongGap(kStep * c.gap, kStep);
        const FgParams base = longGap ? FrameGen::ForLongGap(defaults) : defaults;
        const FgParams mine = longGap ? FrameGen::ForLongGap(learned) : learned;
        double sum[4] = {};
        int n = 0;
        for (int k = 0; k + c.gap < (int)frames.size(); k += 2) {
            fg.TestLoadFrame(1, frames[k].data(), w, h);
            fg.TestLoadFrame(2, frames[k + c.gap].data(), w, h);
            fg.TestLoadFrame(3, frames[k + c.part].data(), w, h);
            sum[0] += fg.TestSynthPsnr(1, 2, 3, t, base, false, kStep * c.gap).psnr;
            sum[1] += mapReady ? fg.TestSynthPsnr(1, 2, 3, t, base, true, kStep * c.gap).psnr : 0;
            sum[2] += fg.TestSynthPsnr(1, 2, 3, t, mine, false, kStep * c.gap).psnr;
            sum[3] += mapReady ? fg.TestSynthPsnr(1, 2, 3, t, mine, true, kStep * c.gap).psnr : 0;
            n++;
        }
        const double k = 1.0 / n;
        printf("%-6d %-6.2f %9.2f %10.2f %12.2f %12.2f\n", c.gap, t, sum[0] * k, sum[1] * k, sum[2] * k, sum[3] * k);
        fflush(stdout);
        profileCsv << c.gap << "," << t << "," << sum[0] * k << "," << sum[1] * k << "," << sum[2] * k << "," << sum[3] * k << "\n";
    }
    fg.SetProfile(nullptr);
    return 0;
}

int RunRifeBench(const std::wstring& outDir)
{
    MakeDirectories(outDir);
    RifeEngine engine(RifeEngine::DefaultDirectory());
    std::ofstream csv(outDir + L"\\rife_bench.csv");
    csv << "w,h,runs,total_ms,run_ms,min_run_ms\n";
    printf("[RIFE 속도] 입력 크기별 신경망 시간 (ms)\n%-10s %8s %8s %8s\n", "크기", "전체", "실행", "최소");
    const int kRuns = 30;
    for (int h : { 128, 192, 256, 384, 576 }) {
        const int w = std::max(64, (int)std::lround(h * 16.0 / 9.0 / 64.0) * 64);
        engine.SetInputSize(w, h);
        int activeW = 0, activeH = 0;
        while (engine.GetState() != RifeEngine::STATE_FAILED && !(engine.ActiveSize(activeW, activeH) && activeW == w && activeH == h)) Sleep(10);
        if (engine.GetState() != RifeEngine::STATE_READY) {
            printf("RIFE를 쓸 수 없습니다: %s\n", engine.GetError().c_str());
            return 1;
        }
        // 무늬를 조금씩 옮긴 두 장. 내용은 시간에 거의 영향이 없다.
        std::vector<float> a((size_t)w * h * 3), b(a.size());
        for (int c = 0; c < 3; c++)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const size_t i = ((size_t)c * h + y) * w + x;
                    a[i] = 0.5f + 0.5f * std::sin(x * 0.11f + y * 0.07f + c);
                    b[i] = 0.5f + 0.5f * std::sin((x - 3) * 0.11f + y * 0.07f + c);
                }
        double total = 0, run = 0, minRun = 1e9;
        for (int i = 0; i < kRuns; i++) {
            RifeEngine::Result r;
            if (!engine.RunBlocking(a, b, w, h, r)) {
                printf("RIFE 실행 실패: %s\n", engine.GetError().c_str());
                return 1;
            }
            engine.Recycle(std::move(r.planes));
            if (i < 3) continue;  // 처음 몇 번은 버린다
            total += r.ms;
            run += r.runMs;
            minRun = std::min(minRun, r.runMs);
        }
        const double k = 1.0 / (kRuns - 3);
        printf("%4dx%-5d %8.2f %8.2f %8.2f\n", w, h, total * k, run * k, minRun);
        csv << w << "," << h << "," << kRuns - 3 << "," << total * k << "," << run * k << "," << minRun << "\n";
    }
    return 0;
}

// 영역 계산 시간 비교: 슬로모션 덤프의 게임 프레임을 화면 전체와 영역으로 번갈아 처리하며 GPU 시간을 잰다.
// 실시간 기록은 게임 화면의 움직임과 GPU 클럭에 따라 흔들리므로, 같은 프레임을 번갈아 넣어 그 영향을 줄인다.
int RunFrameGenRegionBench(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                           const float region[4])
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dumpDir + L"\\frame_*_real.bmp").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName);
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(names.begin(), names.end());
    std::vector<winrt::com_ptr<ID3D11Texture2D>> textures;
    int w = 0, h = 0;
    std::vector<uint32_t> previous;
    for (auto& name : names) {
        std::vector<uint32_t> rgba;
        int fw, fh;
        if (!LoadBmp(dumpDir + L"\\" + name, rgba, fw, fh) || rgba == previous) continue;
        if (w && (fw != w || fh != h)) continue;
        w = fw;
        h = fh;
        std::vector<uint32_t> bgra(rgba.size());
        for (size_t i = 0; i < rgba.size(); i++) {
            const uint32_t c = rgba[i];
            bgra[i] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { bgra.data(), (UINT)w * 4, 0 };
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(device->CreateTexture2D(&td, &data, tex.put()));
        textures.push_back(tex);
        previous = std::move(rgba);
    }
    if (textures.size() < 4) {
        printf("진짜 프레임이 부족합니다: %zu장\n", textures.size());
        return 1;
    }
    MakeDirectories(outDir);

    // GPU 시간: 타임스탬프 쿼리로 감싸고 결과가 나올 때까지 기다린다 (실시간이 아니므로 기다려도 된다).
    D3D11_QUERY_DESC disjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, stampDesc = { D3D11_QUERY_TIMESTAMP, 0 };
    winrt::com_ptr<ID3D11Query> disjoint, begin, end;
    winrt::check_hresult(device->CreateQuery(&disjointDesc, disjoint.put()));
    winrt::check_hresult(device->CreateQuery(&stampDesc, begin.put()));
    winrt::check_hresult(device->CreateQuery(&stampDesc, end.put()));
    auto timed = [&](const std::function<void()>& work) {
        context->Begin(disjoint.get());
        context->End(begin.get());
        work();
        context->End(end.get());
        context->End(disjoint.get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 t0 = 0, t1 = 0;
        while (context->GetData(disjoint.get(), &dj, sizeof(dj), 0) != S_OK) {}
        while (context->GetData(begin.get(), &t0, sizeof(t0), 0) != S_OK) {}
        while (context->GetData(end.get(), &t1, sizeof(t1), 0) != S_OK) {}
        return dj.Disjoint || dj.Frequency == 0 ? -1.0 : (double)(t1 - t0) * 1000.0 / dj.Frequency;
    };

    FrameGen full(device, context), part(device, context);
    FgOptions fullOptions, partOptions;
    fullOptions.learn = partOptions.learn = false;
    fullOptions.refreshHz = partOptions.refreshHz = 240;  // 게임 프레임 사이에 넣을 자리가 있게 한다
    partOptions.regionX = (int)std::lround(region[0] * w);
    partOptions.regionY = (int)std::lround(region[1] * h);
    partOptions.regionW = (int)std::lround((region[0] + region[2]) * w) - partOptions.regionX;
    partOptions.regionH = (int)std::lround((region[1] + region[3]) * h) - partOptions.regionY;
    const double interval = 1.0 / 60.0;
    const int kWarmup = 60, kIterations = 400;
    std::vector<double> est[2], syn[2];
    std::ofstream csv(outDir + L"\\fg_regionbench.csv");
    csv << "iteration,kind,estimate_ms,synth_ms\n";
    printf("[영역 계산 시간] %dx%d, 영역 (%d, %d) %dx%d, 게임 프레임 %zu장을 번갈아 %d번\n", w, h, partOptions.regionX, partOptions.regionY,
           partOptions.regionW, partOptions.regionH, textures.size(), kIterations);
    for (int i = 0; i < kWarmup + kIterations; i++) {
        ID3D11Texture2D* tex = textures[i % textures.size()].get();
        const double time = i * interval;
        // 어느 쪽을 먼저 돌리는지도 번갈아 바꿔 순서의 영향을 없앤다.
        for (int order = 0; order < 2; order++) {
            const int kind = (i + order) % 2;  // 0: 화면 전체, 1: 영역
            FrameGen& fg = kind == 0 ? full : part;
            const FgOptions& options = kind == 0 ? fullOptions : partOptions;
            const double e = timed([&] { fg.PushFrame(tex, w, h, time, options); });
            // 표시 두 번 (게임 프레임 하나에 생성 프레임 두 장을 넣는 경우)
            double s = 0;
            for (int k = 1; k <= 2; k++) s += timed([&] { fg.Render(time + interval * k / 3.0, options); });
            if (i < kWarmup || e < 0 || s < 0) continue;
            est[kind].push_back(e);
            syn[kind].push_back(s / 2);
            csv << i - kWarmup << "," << (kind == 0 ? "full" : "region") << "," << e << "," << s / 2 << "\n";
        }
    }
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const double area = (double)partOptions.regionW * partOptions.regionH / ((double)w * h);
    const double e0 = median(est[0]), e1 = median(est[1]), s0 = median(syn[0]), s1 = median(syn[1]);
    printf("%-10s %14s %14s\n", "", "움직임 찾기", "합성 (1장)");
    printf("%-10s %11.3f ms %11.3f ms\n", "화면 전체", e0, s0);
    printf("%-10s %11.3f ms %11.3f ms\n", "영역", e1, s1);
    printf("넓이 %.0f%% -> 움직임 찾기 %.0f%%, 합성 %.0f%% (화면 전체 대비, 중앙값)\n", area * 100, e0 > 0 ? e1 / e0 * 100 : 0, s0 > 0 ? s1 / s0 * 100 : 0);
    std::ofstream summary(outDir + L"\\fg_regionbench_summary.csv");
    summary << "width,height,region_w,region_h,area,full_estimate_ms,region_estimate_ms,full_synth_ms,region_synth_ms\n";
    summary << w << "," << h << "," << partOptions.regionW << "," << partOptions.regionH << "," << area << "," << e0 << "," << e1 << "," << s0 << ","
            << s1 << "\n";
    return 0;
}

int RunFrameGenFeatureTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir)
{
    MakeDirectories(outDir);
    NoiseTexture noise;
    FrameGen fg(device, context);
    RunFeatureTests(fg, MakeScenes(noise), MakeExtraScenes(noise), outDir);
    return 0;
}

int RunFrameGenPaceTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& outDir)
{
    MakeDirectories(outDir);
    NoiseTexture noise;
    auto scenes = MakeScenes(noise);
    const Scene& pan = scenes[0].shade;
    const int w = 320, h = 180;
    auto makeTexture = [&](const std::vector<uint32_t>& rgba) {
        std::vector<uint32_t> bgra(rgba.size());
        for (size_t i = 0; i < rgba.size(); i++) {
            uint32_t c = rgba[i];
            bgra[i] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { bgra.data(), (UINT)w * 4, 0 };
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(device->CreateTexture2D(&td, &data, tex.put()));
        return tex;
    };

    std::ofstream csv(outDir + L"\fg_pace.csv");
    csv << "features,refresh_hz,drop_every,game_fps,generated_pct,step_error_pct,stalls_per_min,backs_per_min,jumps_per_min\n";
    printf("[표시 간격] 화면마다 보여 준 게임 속 시간의 걸음 (이상적으로는 화면 한 칸만큼씩)\n");
    printf("%6s %8s %8s %6s %10s %12s %10s %10s %10s\n", "처리", "주사율", "게임fps", "빠뜨림", "생성 비율", "걸음 오차", "멈춤/분", "뒤로/분", "건너뜀/분");
    // drop: 게임이 이 장 수마다 한 장을 빠뜨린다 (0이면 빠뜨리지 않음)
    const struct { double hz, fps; int drop; } cases[] = { { 60, 30, 0 }, { 60, 40, 0 }, { 60, 45, 0 }, { 60, 50, 0 }, { 60, 30, 40 }, { 60, 45, 40 },
                                                           { 144, 30, 0 }, { 144, 40, 0 }, { 144, 48, 0 }, { 144, 60, 0 }, { 144, 72, 0 },
                                                           { 144, 90, 0 }, { 144, 60, 40 } };
    for (auto& c : cases)
      for (int features : { 0, (int)FG_FEATURE_ALL }) {
        FrameGen fg(device, context);
        fg.SetFeatures(features);
        FgOptions options;
        options.learn = false;
        options.refreshHz = c.hz;
        const double vsync = 1.0 / c.hz;
        const double seconds = 8.0, warmup = 2.0;
        const double base = 100.0;  // 시각이 0 근처면 시작 처리와 겹치므로 띄운다
        std::vector<double> trueTimes;  // 받아들인 순서대로 게임이 그 프레임을 그린 실제 시각
        int next = 0;
        int counted = 0, generated = 0, stalls = 0, backs = 0, jumps = 0;
        double errorSum = 0, lastContent = -1;
        const int refreshes = (int)(seconds * c.hz);
        for (int n = 1; n <= refreshes; n++) {
            const double now = base + n * vsync;
            // 게임 프레임 k는 k/fps에 그려지고, 다음 화면 갱신에 캡처된다 (캡처 시각 = 그 화면 갱신 시각).
            while (std::ceil((next / c.fps + 0.002) / vsync - 1e-9) <= n) {
                if (c.drop > 0 && next % c.drop == c.drop / 2) {
                    next++;  // 이 장은 게임이 그리지 못했다
                    continue;
                }
                auto tex = makeTexture(Render(pan, (float)(next * 30.0 / c.fps), w, h));
                fg.PushFrame(tex.get(), w, h, now, options);
                trueTimes.push_back(next / c.fps);
                next++;
            }
            fg.Render(now, options);
            const FgStats& st = fg.Stats();
            if (n * vsync < warmup || st.contentFrame < 1) continue;
            const double index = st.contentFrame - 1;
            const int i0 = std::clamp((int)std::floor(index), 0, (int)trueTimes.size() - 1);
            const double frac = index - i0;
            const double t0 = trueTimes[i0];
            const double t1 = i0 + 1 < (int)trueTimes.size() ? trueTimes[i0 + 1] : t0 + 1.0 / c.fps;
            const double content = t0 + (t1 - t0) * frac;
            if (lastContent >= 0 && frac > 0.005 && frac < 0.995) generated++;
            if (lastContent >= 0) {
                const double step = content - lastContent;
                errorSum += std::abs(step - vsync) / vsync;
                if (step < -0.0005) backs++;
                else if (step < 0.2 * vsync) stalls++;
                else if (step > 1.8 * vsync) jumps++;
                counted++;
            }
            lastContent = content;
        }
        const double minutes = (seconds - warmup) / 60.0;
        const double genPct = 100.0 * generated / std::max(counted, 1), errPct = 100.0 * errorSum / std::max(counted, 1);
        printf("%6s %8.0f %8.0f %6s %9.1f%% %11.1f%% %10.0f %10.0f %10.0f\n", features ? "새" : "이전", c.hz, c.fps, c.drop ? "있음" : "", genPct, errPct,
               stalls / minutes, backs / minutes, jumps / minutes);
        fflush(stdout);
        csv << (features ? "new" : "old") << "," << c.hz << "," << c.drop << "," << c.fps << "," << genPct << "," << errPct << "," << stalls / minutes << "," << backs / minutes << "," << jumps / minutes << "\n";
    }
    return 0;
}

// 연속 재생 시험: 녹화의 진짜 프레임을 step장씩 건너뛰어 실제 프로그램처럼 차례로 넣고(고정 픽셀 기록을 먼저 갱신한 뒤 합성),
// 건너뛴 가운데 장과 비교한다. 처리 조합(modes)마다 화면 전체, 고정 픽셀(세 장이 같은 픽셀), 지정한 상자의 결과와 GPU 시간을 낸다.
// 프로필을 주면 그 고정 UI 상자로 시작하고, 끝나면 이번에 찾은 상자를 합쳐 같은 파일에 저장한다 (두 번 돌려 다음 확대를 흉내 낸다).
int RunFrameGenSeqTest(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dumpDir, const std::wstring& outDir,
                       const SeqTestOptions& opt)
{
    std::vector<std::vector<uint32_t>> frames;
    int w = 0, h = 0;
    {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dumpDir + L"\\frame_*_real.bmp").c_str(), &fd);
        std::vector<std::wstring> names;
        if (find != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName);
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end());
        for (auto& name : names) {
            std::vector<uint32_t> rgba;
            int fw, fh;
            if (!LoadBmp(dumpDir + L"\\" + name, rgba, fw, fh)) continue;
            if (!frames.empty() && frames.back() == rgba) continue;
            w = fw;
            h = fh;
            frames.push_back(std::move(rgba));
        }
    }
    const int step = std::max(2, opt.step & ~1);
    if ((int)frames.size() < step * 4) {
        printf("진짜 프레임이 부족합니다: %zu장\n", frames.size());
        return 1;
    }
    MakeDirectories(outDir);
    double kStep = 1.0 / 60.0;
    {
        std::ifstream timesFile(dumpDir + L"\\real_times.txt");
        std::vector<double> times, gaps;
        for (double v; timesFile >> v;) times.push_back(v);
        for (size_t i = 1; i < times.size(); i++) gaps.push_back(times[i] - times[i - 1]);
        if (gaps.size() >= 4) {
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            kStep = gaps[gaps.size() / 2];
        }
    }
    const double pairGap = kStep * step;
    FrameGen fg(device, context);
    FgParams defaults;
    GameProfile profile;
    if (!opt.profile.empty()) profile.Load(opt.profile);
    printf("진짜 프레임 %zu장 (%dx%d), 쌍 간격 %.1f ms, 시작할 때 고정 UI 상자 %zu개\n", frames.size(), w, h, pairGap * 1000,
           profile.stillBoxes.size());
    for (const StillBox& b : profile.stillBoxes)
        printf("  상자 x %.3f y %.3f 너비 %.3f 높이 %.3f 확신도 %.0f\n", b.x, b.y, b.w, b.h, b.confidence);

    fg.TestLoadFrame(0, frames[0].data(), w, h);  // 텍스처 크기를 정한다
    fg.SetProfile(&profile);
    fg.TestClearStill();

    const size_t nModes = opt.modes.size(), nBoxes = opt.boxes.size();
    struct Acc { double db = 0, staticDb = 0, est = 0, syn = 0; long long broken = 0, px = 0; int n = 0, nStatic = 0; std::vector<double> boxDb; };
    std::vector<std::array<Acc, 3>> acc(nModes);
    for (auto& a : acc) for (auto& s : a) s.boxDb.assign(nBoxes, 0.0);
    std::ofstream csv(outDir + L"\\fg_seqtest.csv");
    csv << "pair,k,mode,db,static_px,broken";
    for (auto& b : opt.boxes) csv << "," << b.name << "_db";
    csv << ",estimate_ms,synth_ms\n";

    auto mseOf = [](const std::vector<uint32_t>& out, const std::vector<uint32_t>& ref, int w, int x0, int y0, int x1, int y1) {
        double sum = 0;
        long long n = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                const size_t i = (size_t)y * w + x;
                for (int c = 0; c < 3; c++) {
                    const int d = (int)((out[i] >> (c * 8)) & 255) - (int)((ref[i] >> (c * 8)) & 255);
                    sum += d * d;
                }
                n += 3;
            }
        return n ? sum / n : 0.0;
    };
    auto dbOf = [](double mse) { return mse > 1e-9 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0; };

    int pair = 0, sinceGrid = 0;
    const int loops = std::max(1, opt.loops);
    const int pairsPerLoop = ((int)frames.size() - 1 - step) / step + 1;
    for (int loop = 0; loop < loops; loop++)
    for (int k = 0; k + step < (int)frames.size(); k += step, pair++) {
        const auto& A = frames[k];
        const auto& B = frames[k + step];
        const auto& T = frames[k + step / 2];
        fg.TestLoadFrame(0, A.data(), w, h);
        fg.TestLoadFrame(1, B.data(), w, h);
        fg.TestLoadFrame(2, T.data(), w, h);
        // 실제 프로그램처럼 이 쌍으로 고정 픽셀 기록을 먼저 갱신하고 합성한다.
        fg.SetFeatures(FG_FEATURE_ALL);
        fg.TestUpdateStill(0, 1);
        if (++sinceGrid >= opt.gridEvery) {
            fg.TestStillGrid(sinceGrid * pairGap);
            sinceGrid = 0;
        }
        std::vector<uint8_t> mask(A.size());
        for (size_t i = 0; i < mask.size(); i++)
            mask[i] = ((A[i] ^ T[i]) & 0xFFFFFF) == 0 && ((B[i] ^ T[i]) & 0xFFFFFF) == 0;
        const bool save = std::find(opt.save.begin(), opt.save.end(), pair) != opt.save.end();
        for (size_t m = 0; m < nModes; m++) {
            fg.SetFeatures(opt.modes[m]);
            fg.TestResetTimers();
            const auto r = fg.TestSynthPsnr(0, 1, 2, 0.5f, defaults, false, pairGap);
            const auto out = fg.TestReadOutput();
            double sm = 0;
            long long broken = 0, count = 0;
            for (size_t i = 0; i < out.size(); i++) {
                if (!mask[i]) continue;
                int worst = 0;
                for (int c = 0; c < 3; c++) {
                    const int d = (int)((out[i] >> (c * 8)) & 255) - (int)((T[i] >> (c * 8)) & 255);
                    sm += d * d / 3.0;
                    worst = std::max(worst, std::abs(d));
                }
                broken += worst > 8;
                count++;
            }
            Acc& a = acc[m][loop > 0 ? 2 : pair >= 48 ? 1 : 0];
            a.db += r.psnr;
            if (count) {
                a.staticDb += dbOf(sm / count);
                a.nStatic++;
            }
            a.broken += broken;
            a.px += count;
            a.est += r.estimateMs;
            a.syn += r.synthMs;
            a.n++;
            csv << pair << "," << k << "," << opt.modes[m] << "," << r.psnr << "," << count << "," << broken;
            for (size_t b = 0; b < nBoxes; b++) {
                const auto& bx = opt.boxes[b];
                const double db = dbOf(mseOf(out, T, w, bx.x, bx.y, std::min(w, bx.x + bx.w), std::min(h, bx.y + bx.h)));
                a.boxDb[b] += db;
                csv << "," << db;
            }
            csv << "," << r.estimateMs << "," << r.synthMs << "\n";
            if (save) {
                wchar_t name[64];
                swprintf_s(name, L"\\seq_%03d_mode%d.bmp", pair, opt.modes[m]);
                SaveBmp(outDir + name, out, w, h);
            }
        }
        if (save) {
            wchar_t name[64];
            swprintf_s(name, L"\\seq_%03d_truth.bmp", pair);
            SaveBmp(outDir + name, T, w, h);
            swprintf_s(name, L"\\seq_%03d_a.bmp", pair);
            SaveBmp(outDir + name, A, w, h);
            swprintf_s(name, L"\\seq_%03d_b.bmp", pair);
            SaveBmp(outDir + name, B, w, h);
            // 보호 상태를 정답 위에 칠한다. 하늘색: 둘레도 멈춰 있어 보호, 분홍: 둘레가 계속 움직여 보호,
            // 초록: 화면이 움직이는 동안 오래 그대로라 둘레와 상관없이 보호(고정 UI 증거), 노랑: 오래 같았지만 보호하지 않음.
            const auto still = fg.TestReadStill();
            std::vector<uint32_t> vis(T.size());
            for (size_t i = 0; i < vis.size(); i++) {
                const uint32_t c = T[i];
                float rgb[3] = { (c & 255) * 0.35f, ((c >> 8) & 255) * 0.35f, ((c >> 16) & 255) * 0.35f };
                int diff = 0;
                for (int ch = 0; ch < 3; ch++)
                    diff = std::max(diff, std::abs((int)((A[i] >> (ch * 8)) & 255) - (int)((B[i] >> (ch * 8)) & 255)));
                const float cnt = (float)(still[i] & 0xFF), moving = (float)((still[i] >> 8) & 0xFFFF) / 65535.0f;
                const float ui = std::clamp(((float)(still[i] >> 24) - 96.0f) / 32.0f, 0.0f, 1.0f);
                const float old = std::clamp((cnt - 40.0f) / 8.0f, 0.0f, 1.0f);
                const float quiet = std::clamp((0.05f - moving) / 0.03f, 0.0f, 1.0f), busy = std::clamp((moving - 0.7f) / 0.1f, 0.0f, 1.0f);
                float color[3] = { 0, 0, 0 }, amount = 0;
                if (diff < 4) {
                    const float base = old * std::max(quiet, busy);
                    if (ui > base && ui > 0) { color[0] = 90; color[1] = 230; color[2] = 90; amount = ui; }
                    else if (base > 0 && quiet >= busy) { color[0] = 80; color[1] = 200; color[2] = 255; amount = base; }
                    else if (base > 0) { color[0] = 255; color[1] = 90; color[2] = 200; amount = base; }
                    else if (old > 0) { color[0] = 255; color[1] = 220; color[2] = 60; amount = old * 0.6f; }
                }
                for (int ch = 0; ch < 3; ch++) rgb[ch] = rgb[ch] * (1 - amount) + color[ch] * amount;
                vis[i] = 0xFF000000u | (uint32_t)rgb[0] | ((uint32_t)rgb[1] << 8) | ((uint32_t)rgb[2] << 16);
            }
            swprintf_s(name, L"\\seq_%03d_still.bmp", pair);
            SaveBmp(outDir + name, vis, w, h);
        }
    }
    if (sinceGrid > 0) fg.TestStillGrid(sinceGrid * pairGap);

    printf("\n%-6s %-8s %5s %8s %8s %8s %8s %8s", "조합", "구간", "쌍", "전체dB", "고정dB", "깨짐%", "추정ms", "합성ms");
    for (auto& b : opt.boxes) printf(" %10s", b.name.c_str());
    printf("\n");
    const char* part[3] = { "앞 48쌍", "첫 회 뒤", "반복" };
    printf("(한 회 %d쌍, %d회 반복)\n", pairsPerLoop, loops);
    for (size_t m = 0; m < nModes; m++)
        for (int p = 0; p < 3; p++) {
            const Acc& a = acc[m][p];
            if (!a.n) continue;
            printf("%-6d %-8s %5d %8.2f %8.2f %8.3f %8.3f %8.3f", opt.modes[m], part[p], a.n, a.db / a.n, a.nStatic ? a.staticDb / a.nStatic : 0.0,
                   a.px ? 100.0 * a.broken / a.px : 0.0, a.est / a.n, a.syn / a.n);
            for (size_t b = 0; b < nBoxes; b++) printf(" %10.2f", a.boxDb[b] / a.n);
            printf("\n");
        }
    const auto found = fg.FoundStillBoxes();
    printf("\n이번에 찾은 고정 UI 상자 %zu개\n", found.size());
    for (const StillBox& b : found)
        printf("  x %.3f y %.3f 너비 %.3f 높이 %.3f (픽셀 %d,%d %dx%d)\n", b.x, b.y, b.w, b.h, (int)std::lround(b.x * w), (int)std::lround(b.y * h),
               (int)std::lround(b.w * w), (int)std::lround(b.h * h));
    printf("합친 뒤 프로필 상자 %zu개\n", profile.stillBoxes.size());
    for (const StillBox& b : profile.stillBoxes)
        printf("  x %.3f y %.3f 너비 %.3f 높이 %.3f 확신도 %.0f\n", b.x, b.y, b.w, b.h, b.confidence);
    if (!opt.profile.empty()) profile.SaveTo(opt.profile);
    fflush(stdout);
    return 0;
}
