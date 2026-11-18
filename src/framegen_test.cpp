// 프레임 생성 자체 테스트 (--fg-selftest 폴더)
//
// 계산으로 만든 움직이는 영상에서 가운데 프레임을 빼고 양옆 프레임으로 다시 만든 뒤 실제 프레임과 PSNR로 비교한다.
// 화면 캡처나 마우스 없이 GPU만 쓰므로 언제든 같은 결과를 얻을 수 있다.

#include "framegen_test.h"

#include <windows.h>

#include <algorithm>
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
    csv << "gap,t,samples,hold_db,blend_db,block_db,block_longgap_db,rife_db,frame_ms\n";
    bool rifeOk = true;
    const struct { int gap, part; } cases[] = { { 2, 1 }, { 3, 1 }, { 4, 2 }, { 4, 1 }, { 6, 3 }, { 8, 4 } };
    for (auto& c : cases) {
        const float t = (float)c.part / c.gap;
        const FgParams longGap = FrameGen::IsLongGap(kStep * c.gap, kStep) ? FrameGen::ForLongGap(defaults) : defaults;
        double hold = 0, blend = 0, block = 0, blockLong = 0, rife = 0;
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
            block += fg.TestSynthPsnr(1, 2, 3, t, defaults, false, kStep * c.gap).psnr;
            if (save) SaveBmp(name + L"block.bmp", fg.TestReadOutput(), w, h);
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
        printf("%-6d %-6.2f %8.2f %9.2f %9.2f %10.2f %9.2f\n", c.gap, t, hold * k, blend * k, block * k, blockLong * k, rife * rk);
        fflush(stdout);
        csv << c.gap << "," << t << "," << n << "," << hold * k << "," << blend * k << "," << block * k << "," << blockLong * k << ","
            << rife * rk << "," << kStep * 1000 << "\n";
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
