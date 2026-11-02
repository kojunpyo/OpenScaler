// 프레임 생성 자체 테스트 (--fg-selftest 폴더)
//
// 계산으로 만든 움직이는 영상에서 가운데 프레임을 빼고 양옆 프레임으로 다시 만든 뒤 실제 프레임과 PSNR로 비교한다.
// 화면 캡처나 마우스 없이 GPU만 쓰므로 언제든 같은 결과를 얻을 수 있다.

#include "framegen_test.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <ppl.h>
#include <functional>
#include <string>
#include <vector>

#include "framegen.h"

namespace {

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
    CreateDirectoryW(outDir.c_str(), nullptr);
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
            fg.TestSynthPsnr(0, 1, 2, 0.5f, profile.params, false, kInterval);
            fg.TestAddFlowSample(profile.params, kInterval);
            fg.TestUpdateMapTexture();
            float current = fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.params, false, kInterval * 2).psnr;
            float candidate = fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.Candidate(), false, kInterval * 2).psnr;
            float blend = fg.TestBlendPsnr(0, 2, 1, 0.5f);
            profile.AddEvaluation(current, candidate, blend);
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
            after += fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.params, false, kInterval * 2).psnr;
            withMap += fg.TestSynthPsnr(0, 2, 1, 0.5f, profile.params, true, kInterval * 2).psnr;
        }
        before /= kHeldOut;
        after /= kHeldOut;
        withMap /= kHeldOut;
        printf("%-10s %9.2f %9.2f %9.2f %6d %-8s 칸 %d, 범위 %d, 벌점 %.4f, 허용 %.2f\n", scene.key, before, after, withMap, profile.adoptedCount,
               kGenreKeys[profile.genre], profile.params.cellPx, profile.params.coarseRadius, profile.params.smoothness, profile.params.blendTol);
        fflush(stdout);
        learn << scene.key << "," << before << "," << after << "," << withMap << "," << profile.adoptedCount << "," << kGenreKeys[profile.genre]
              << "," << profile.params.cellPx << "," << profile.params.coarseRadius << "," << profile.params.smoothness << ","
              << profile.params.blendTol << ",";
        for (size_t i = 0; i < profile.history.size(); i++) learn << (i ? ";" : "") << profile.history[i];
        learn << "\n";
        fg.SetProfile(nullptr);
    }

    return 0;
}
