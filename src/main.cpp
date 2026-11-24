// OpenScaler - 창 모드 게임을 캡처해 전체화면으로 업스케일하는 프로토타입
//
// 흐름: 대상 창 캡처(Windows.Graphics.Capture)
//       -> 클라이언트 영역만 잘라 복사
//       -> Pass 1: 업스케일 셰이더 -> Pass 2: 샤프닝 셰이더 -> 커서 그리기
//          (FSR 1.0 모드에서는 Pass 1이 EASU, Pass 2가 RCAS)
//       -> 클릭이 통과하는 최상위 전체화면 창에 출력
//
// 커서 처리: 확대된 화면 위에 커서가 올라오면 실제 커서는 원본 창의 대응 위치로 옮기고
//            시스템 커서는 숨긴 뒤, 확대된 위치에 커서를 직접 그린다.
//            오버레이는 클릭이 통과하므로 클릭은 원본 창의 올바른 위치에 전달된다.

#include <windows.h>
#include <unknwn.h>
#include <dwmapi.h>
#include <magnification.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app.h"
#include "framegen.h"
#include "framegen_test.h"
#include "game_profile.h"
#include "gui.h"
#include "shaders.h"
#include "fsr_constants.h"
#include "resource.h"
#include "PSEasu.h"  // 빌드 때 fxc가 만든 FSR 셰이더 바이트코드
#include "PSRcas.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::com_ptr;
using winrt::check_hresult;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// 설정
// ---------------------------------------------------------------------------

const char* const kModeNames[MODE_COUNT] = { "Nearest", "Bilinear", "Bicubic", "Lanczos2", "FSR 1.0" };

enum HotkeyId { HK_TOGGLE = 1, HK_MENU, HK_MODE, HK_INTEGER, HK_SHARP_UP, HK_SHARP_DOWN, HK_COMPARE, HK_OSD, HK_CURSOR_SPEED, HK_FRAMEGEN, HK_SLOWMO, HK_SLOWMO_PAUSE, HK_SLOWMO_PREV, HK_SLOWMO_NEXT, HK_QUIT };

// HLSL cbuffer Params와 같은 레이아웃 (16바이트 정렬)
struct ShaderParams {
    float srcSize[2];
    float invSrcSize[2];
    int mode;
    float sharpness;
    float pad[2];
};

// ---------------------------------------------------------------------------
// 전역 상태
// ---------------------------------------------------------------------------

static Settings g_settings;

// 설정 파일: 실행 파일 옆의 OpenScaler.ini. 단축키로 바꾼 설정을 저장했다가 다음 실행 때 불러온다.
// 비교 모드는 발표/확인용이라 저장하지 않는다.
static const wchar_t* kModeKeys[MODE_COUNT] = { L"nearest", L"bilinear", L"bicubic", L"lanczos2", L"fsr" };
static const wchar_t* kSettingsSection = L"Settings";
static std::wstring g_settingsPath;

static std::wstring GetSettingsPath()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t dot = p.find_last_of(L'.');
    return (dot == std::wstring::npos ? p : p.substr(0, dot)) + L".ini";
}

static void LoadSettings()
{
    const wchar_t* file = g_settingsPath.c_str();
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t buf[64];
    GetPrivateProfileStringW(kSettingsSection, L"Mode", L"", buf, ARRAYSIZE(buf), file);
    for (int i = 0; i < MODE_COUNT; i++)
        if (_wcsicmp(buf, kModeKeys[i]) == 0) g_settings.mode = i;

    GetPrivateProfileStringW(kSettingsSection, L"Sharpness", L"", buf, ARRAYSIZE(buf), file);
    if (buf[0]) g_settings.sharpness = std::clamp((float)_wtof(buf), 0.0f, 1.0f);

    g_settings.integerScale = GetPrivateProfileIntW(kSettingsSection, L"IntegerScale", g_settings.integerScale, file) != 0;
    g_settings.osdAlways = GetPrivateProfileIntW(kSettingsSection, L"AlwaysShowStatus", g_settings.osdAlways, file) != 0;
    g_settings.adjustCursorSpeed =
        GetPrivateProfileIntW(kSettingsSection, L"AdjustCursorSpeed", g_settings.adjustCursorSpeed, file) != 0;

    g_settings.frameGen = GetPrivateProfileIntW(kSettingsSection, L"FrameGen", g_settings.frameGen, file) != 0;
    int multiplier = GetPrivateProfileIntW(kSettingsSection, L"FgMultiplier", g_settings.fgMultiplier, file);
    g_settings.fgMultiplier = multiplier >= 2 && multiplier <= 4 ? multiplier : 0;
    g_settings.fgQuality = std::clamp((int)GetPrivateProfileIntW(kSettingsSection, L"FgQuality", g_settings.fgQuality, file), 0, FG_QUALITY_COUNT - 1);
    g_settings.fgMethod = std::clamp((int)GetPrivateProfileIntW(kSettingsSection, L"FgMethod", g_settings.fgMethod, file), 0, FG_METHOD_COUNT - 1);
    g_settings.fgGenerator = std::clamp((int)GetPrivateProfileIntW(kSettingsSection, L"FgGenerator", g_settings.fgGenerator, file), 0, FG_GENERATOR_COUNT - 1);
    {
        const int preset = (int)GetPrivateProfileIntW(kSettingsSection, L"FgPreset", g_settings.fgPreset, file);
        g_settings.fgPreset = preset == FG_PRESET_NO_MAP ? FG_PRESET_NO_MAP : FG_PRESET_AUTO;
    }
    g_settings.fgLearn = GetPrivateProfileIntW(kSettingsSection, L"FgLearn", g_settings.fgLearn, file) != 0;
    {
        // 예전 키(SlowMoTargetFps)는 기본값 120이 저장돼 있어 읽지 않는다. 새 키의 기본값은 모니터 주사율이다.
        const int fps = (int)GetPrivateProfileIntW(kSettingsSection, L"SlowMoHz", g_settings.slowMoTargetFps, file);
        g_settings.slowMoTargetFps = fps <= 0 ? 0 : std::clamp(fps, 30, 1000);  // 0: 모니터 주사율
    }
    g_settings.slowMoSeconds = std::clamp((int)GetPrivateProfileIntW(kSettingsSection, L"SlowMoSeconds", g_settings.slowMoSeconds, file), 1, 3);
}

static void SaveSettings()
{
    const wchar_t* file = g_settingsPath.c_str();
    wchar_t buf[32];
    WritePrivateProfileStringW(kSettingsSection, L"Mode", kModeKeys[g_settings.mode], file);
    swprintf_s(buf, L"%.2f", g_settings.sharpness);
    WritePrivateProfileStringW(kSettingsSection, L"Sharpness", buf, file);
    WritePrivateProfileStringW(kSettingsSection, L"IntegerScale", g_settings.integerScale ? L"1" : L"0", file);
    WritePrivateProfileStringW(kSettingsSection, L"AlwaysShowStatus", g_settings.osdAlways ? L"1" : L"0", file);
    WritePrivateProfileStringW(kSettingsSection, L"AdjustCursorSpeed", g_settings.adjustCursorSpeed ? L"1" : L"0", file);
    WritePrivateProfileStringW(kSettingsSection, L"FrameGen", g_settings.frameGen ? L"1" : L"0", file);
    WritePrivateProfileStringW(kSettingsSection, L"FgMultiplier", std::to_wstring(g_settings.fgMultiplier).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"FgQuality", std::to_wstring(g_settings.fgQuality).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"FgMethod", std::to_wstring(g_settings.fgMethod).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"FgGenerator", std::to_wstring(g_settings.fgGenerator).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"FgPreset", std::to_wstring(g_settings.fgPreset).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"FgLearn", g_settings.fgLearn ? L"1" : L"0", file);
    WritePrivateProfileStringW(kSettingsSection, L"SlowMoHz", std::to_wstring(g_settings.slowMoTargetFps).c_str(), file);
    WritePrivateProfileStringW(kSettingsSection, L"SlowMoTargetFps", nullptr, file);
    WritePrivateProfileStringW(kSettingsSection, L"SlowMoSeconds", std::to_wstring(g_settings.slowMoSeconds).c_str(), file);
}

// ---------------------------------------------------------------------------
// 커서 속도 보정
// 확대 중에는 실제 커서가 작은 원본 창 안에서 움직이므로 화면상 속도가 배율만큼 빨라진다.
// 옵션을 켜면 그동안 Windows 마우스 속도(1~20단계)를 낮췄다가 끝나면 되돌린다.
// 레지스트리에는 쓰지 않으므로 로그아웃하면 원래대로 돌아오고, 비정상 종료에 대비해
// 원래 값을 설정 파일(SavedMouseSpeed)에 적어 두었다가 다음 실행 때 복구한다.
// ---------------------------------------------------------------------------

// 단계별 속도 배율 (포인터 정밀도 향상이 꺼진 기준, 10단계가 1배)
static const float kMouseSpeedMultiplier[21] = {
    0, 1 / 32.f, 1 / 16.f, 1 / 8.f, 2 / 8.f, 3 / 8.f, 4 / 8.f, 5 / 8.f, 6 / 8.f, 7 / 8.f, 1.0f,
    1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 2.75f, 3.0f, 3.25f, 3.5f };
static int g_originalMouseSpeed = 0;  // 0이면 바꾸지 않은 상태
static int g_appliedMouseSpeed = 0;

static void SetMouseSpeed(int speed)
{
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(speed)), 0);
}

static void RestoreMouseSpeed()
{
    if (!g_originalMouseSpeed) return;
    SetMouseSpeed(g_originalMouseSpeed);
    g_originalMouseSpeed = 0;
    g_appliedMouseSpeed = 0;
    WritePrivateProfileStringW(kSettingsSection, L"SavedMouseSpeed", nullptr, g_settingsPath.c_str());
}

// 화면상 속도가 원래와 같아지도록, 원래 배율 / 확대 배율에 가장 가까운 단계로 바꾼다.
static void ApplyMouseSpeed(float scale)
{
    if (!g_originalMouseSpeed) {
        int current = 10;
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &current, 0);
        g_originalMouseSpeed = std::clamp(current, 1, 20);
        WritePrivateProfileStringW(kSettingsSection, L"SavedMouseSpeed", std::to_wstring(g_originalMouseSpeed).c_str(),
                                   g_settingsPath.c_str());
    }

    float target = kMouseSpeedMultiplier[g_originalMouseSpeed] / scale;
    int best = 1;
    for (int i = 2; i <= 20; i++)
        if (std::abs(kMouseSpeedMultiplier[i] - target) < std::abs(kMouseSpeedMultiplier[best] - target)) best = i;

    if (best != g_appliedMouseSpeed) {
        SetMouseSpeed(best);
        g_appliedMouseSpeed = best;
    }
}

// 지난번에 속도를 바꾼 채로 비정상 종료됐다면 원래 값으로 되돌린다.
static void RecoverMouseSpeed()
{
    int saved = GetPrivateProfileIntW(kSettingsSection, L"SavedMouseSpeed", 0, g_settingsPath.c_str());
    if (saved < 1 || saved > 20) return;
    g_originalMouseSpeed = saved;
    RestoreMouseSpeed();
    printf("지난 실행에서 바뀐 마우스 속도를 원래 값(%d)으로 되돌렸습니다.\n", saved);
}

// D3D 공용 리소스
static com_ptr<ID3D11Device> g_device;
static com_ptr<ID3D11DeviceContext> g_ctx;
static wgd3d::IDirect3DDevice g_winrtDevice{ nullptr };
static com_ptr<ID3D11VertexShader> g_vs;
static com_ptr<ID3D11PixelShader> g_psScale;
static com_ptr<ID3D11PixelShader> g_psSharpen;
static com_ptr<ID3D11PixelShader> g_psCursor;
static com_ptr<ID3D11PixelShader> g_psEasu;  // FSR 1.0 업스케일
static com_ptr<ID3D11PixelShader> g_psRcas;  // FSR 1.0 샤프닝
static com_ptr<ID3D11SamplerState> g_sampler;
static com_ptr<ID3D11Buffer> g_cbuffer;
static com_ptr<ID3D11Buffer> g_fsrCbuffer;
static com_ptr<ID3D11BlendState> g_blendAlpha;   // 일반 커서 픽셀: 알파 블렌딩
static com_ptr<ID3D11BlendState> g_blendInvert;  // 흑백 커서의 반전 픽셀: 1 - 배경색
static com_ptr<ID3D11RasterizerState> g_rsScissor;  // 비교 모드에서 화면 절반만 그릴 때 사용
static com_ptr<ID3D11ShaderResourceView> g_whiteSrv; // 1x1 흰색 텍스처 (비교 모드 구분선)

// 화면 위 상태 표시(OSD)용 Direct2D / DirectWrite
static com_ptr<ID2D1Factory1> g_d2dFactory;
static com_ptr<ID2D1DeviceContext> g_d2dCtx;
static com_ptr<IDWriteFactory> g_dwFactory;
static Clock::time_point g_osdUntil;  // 이 시각까지 상태를 표시한다

// 프레임 생성: 셰이더 컴파일에 몇 초 걸려 화면이 멈추지 않도록 프로그램을 시작할 때 뒤에서 만들어 둔다.
static std::unique_ptr<FrameGen> g_frameGen;
static std::future<std::unique_ptr<FrameGen>> g_frameGenLoading;
static FILE* g_fgLog = nullptr;  // --fg-log: 화면마다 시점을 기록한다 (테스트용)
static bool g_gpuPriorityAllowed = true;  // --no-gpu-priority: GPU 우선순위를 올리지 않는다 (비교 측정용)
static int g_rifeGpuClass = 2;            // --rife-gpu-priority N: RIFE일 때의 GPU 우선순위 (2 보통, 3 보통 이상, 4 높음, 5 실시간, 측정용)
static int g_gpuPriorityClass = -1;       // 지금 설정한 D3DKMT 우선순위 등급

// --fg-log: 화면마다 GPU 작업 전체(프레임 생성, 업스케일, 상태 표시)가 GPU에서 시작해 끝날 때까지 걸린 시간.
// 게임이 GPU를 함께 쓰면 기다린 시간도 들어간다. 결과는 몇 화면 뒤에 나오므로 화면 번호와 함께 남긴다.
struct FrameGpuTimer {
    winrt::com_ptr<ID3D11Query> disjoint, begin, mid, end;  // mid: 프레임 생성이 끝난 때
    long long frame = -1;
    bool pending = false;
};
static FrameGpuTimer g_frameTimers[8];
static long long g_frameIndex = 0, g_timedFrame = -1;
static double g_timedMs = 0, g_timedFgMs = 0;
static FrameGpuTimer* g_activeTimer = nullptr;
static bool g_timerMidEnded = false;

// 슬로모션 테스트: 짧게 녹화해 목표 fps만큼 채운 뒤, 화면 주사율로 한 장씩 보여 준다.
static struct {
    SlowMoState state = SLOWMO_OFF;
    double startAt = 0;     // 녹화를 시작할 시각
    double recordStart = 0;
    int index = 0;          // 재생 중인 프레임
    int vsyncs = 0;         // 지금 프레임을 보여 준 횟수
    int holdVsyncs = 1;     // 한 프레임을 보여 줄 횟수
    int loopWait = 0;       // 끝에서 잠깐 멈춘 횟수
    bool paused = false;
    bool label = true;
    UINT w = 0, h = 0;
    std::wstring message;   // 실패 이유
    Clock::time_point messageUntil;
    std::wstring dumpDir;   // --slowmo-dump: 만든 프레임을 파일로 남긴다 (테스트용)
} g_slowMo;
static const uint64_t kSlowMoMaxBytes = 4ull << 30;  // 프레임을 담을 메모리 한도
static const int kSlowMoLoopWait = 30;  // 처음으로 돌아가기 전에 마지막 장을 보여 줄 횟수
static void SlowMoStop();
static double NowSeconds();

static bool g_systemCursorHidden = false;

// 커서 모양 하나를 텍스처로 변환한 것
struct CursorImage {
    com_ptr<ID3D11ShaderResourceView> color;
    com_ptr<ID3D11ShaderResourceView> invert;  // 반전 픽셀이 없으면 nullptr
    UINT w = 0, h = 0;
    int hotX = 0, hotY = 0;
};

// 스케일링 세션별 리소스
struct ScalingSession {
    HWND target = nullptr;
    HWND overlay = nullptr;
    RECT monitor{};
    int outW = 0, outH = 0;

    wgc::Direct3D11CaptureFramePool framePool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    com_ptr<IDXGISwapChain1> swapChain;
    com_ptr<ID3D11RenderTargetView> backBufferRtv;
    HANDLE frameWaitable = nullptr;  // 화면에 표시될 준비가 되면 신호가 오는 핸들 (지연 감소용)

    // 잘라낸 원본 (클라이언트 영역)
    com_ptr<ID3D11Texture2D> srcTex;
    com_ptr<ID3D11ShaderResourceView> srcSrv;
    UINT srcW = 0, srcH = 0;

    // 업스케일 결과 (샤프닝 전)
    com_ptr<ID3D11Texture2D> midTex;
    com_ptr<ID3D11RenderTargetView> midRtv;
    com_ptr<ID3D11ShaderResourceView> midSrv;
    UINT midW = 0, midH = 0;

    // 화면 좌표계에서 원본 창 영역과 확대 출력 영역
    RECT srcScreen{};
    RECT dstScreen{};
    bool layoutValid = false;

    // 커서
    bool cursorMapped = false;  // true: 실제 커서가 원본 창 안에 있고 확대 위치에 직접 그리는 중
    std::unordered_map<HCURSOR, CursorImage> cursors;

    // 화면 위 상태 표시
    com_ptr<ID2D1Bitmap1> d2dTarget;  // 백버퍼를 Direct2D에서 그릴 수 있게 감싼 것
    com_ptr<IDWriteTextFormat> textFormat;
    com_ptr<ID2D1SolidColorBrush> textBrush;
    com_ptr<ID2D1SolidColorBrush> bgBrush;

    // 통계
    int capturedFrames = 0;
    int presentedFrames = 0;
    double captureFps = 0;
    double outputFps = 0;
    Clock::time_point statTime;

    // 프레임 생성
    GameProfile profile;
    double refreshHz = 60;
    bool fgWasOn = false;
    Clock::time_point profileSaveTime;
};
static ScalingSession* g_scaling = nullptr;

// ---------------------------------------------------------------------------
// 초기화
// ---------------------------------------------------------------------------

static com_ptr<ID3DBlob> CompileShader(const char* entry, const char* target)
{
    com_ptr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "shaders.hlsl", nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        if (errors) fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        check_hresult(hr);
    }
    return code;
}

static com_ptr<ID3D11PixelShader> CreatePixelShader(const char* entry)
{
    auto code = CompileShader(entry, "ps_5_0");
    com_ptr<ID3D11PixelShader> ps;
    check_hresult(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, ps.put()));
    return ps;
}

static com_ptr<ID3D11BlendState> CreateBlend(D3D11_BLEND src, D3D11_BLEND dst)
{
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = src;
    rt.DestBlend = dst;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    com_ptr<ID3D11BlendState> state;
    check_hresult(g_device->CreateBlendState(&bd, state.put()));
    return state;
}

static com_ptr<ID3D11ShaderResourceView> CreatePixelSrv(const std::vector<uint32_t>& px, UINT w, UINT h);

static void InitD3D()
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, g_device.put(), nullptr, g_ctx.put()));

    // Windows.Graphics.Capture는 WinRT 형식의 D3D 디바이스를 요구한다.
    auto dxgiDevice = g_device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    g_winrtDevice = inspectable.as<wgd3d::IDirect3DDevice>();

    auto vsCode = CompileShader("VSMain", "vs_5_0");
    check_hresult(g_device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, g_vs.put()));
    g_psScale = CreatePixelShader("PSScale");
    g_psSharpen = CreatePixelShader("PSSharpen");
    g_psCursor = CreatePixelShader("PSCursor");
    check_hresult(g_device->CreatePixelShader(g_PSEasu, sizeof(g_PSEasu), nullptr, g_psEasu.put()));
    check_hresult(g_device->CreatePixelShader(g_PSRcas, sizeof(g_PSRcas), nullptr, g_psRcas.put()));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(g_device->CreateSamplerState(&sd, g_sampler.put()));

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hresult(g_device->CreateBuffer(&bd, nullptr, g_cbuffer.put()));
    bd.ByteWidth = sizeof(FsrParams);
    check_hresult(g_device->CreateBuffer(&bd, nullptr, g_fsrCbuffer.put()));

    g_blendAlpha = CreateBlend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    // 셰이더 출력이 흰색(1)이면 1 - 배경, 검은색(0)이면 배경 그대로
    g_blendInvert = CreateBlend(D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_INV_SRC_COLOR);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    check_hresult(g_device->CreateRasterizerState(&rd, g_rsScissor.put()));
    g_whiteSrv = CreatePixelSrv({ 0xFFFFFFFF }, 1, 1);

    // Direct2D를 같은 D3D 디바이스 위에 만들어 백버퍼에 글자를 그린다.
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                    g_d2dFactory.put_void()));
    com_ptr<ID2D1Device> d2dDevice;
    check_hresult(g_d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));
    check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_d2dCtx.put()));
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(g_dwFactory.put())));
}

// ---------------------------------------------------------------------------
// 오버레이 창
// ---------------------------------------------------------------------------

// 확대 화면 위 메뉴 (Alt+Shift+G). 열려 있는 동안 확대 화면 창이 마우스를 받고, 게임 창의 포커스는 그대로 둔다.
static bool g_overlayMenu = false;
// 확대를 시작하면 묻는 "프레임 생성 영역을 지정할까요?"와 그 뒤의 추천 영역 계산
enum RegionWizard { WIZARD_NONE, WIZARD_ASK, WIZARD_COMPUTING, WIZARD_EVERYWHERE };
static int g_wizard = WIZARD_NONE;
static const double kWizardSeconds = 10.0;  // "예"를 누른 뒤 이만큼 모아 추천한다
static bool g_autoOverlay = false;  // 확대를 시작하면 메뉴와 팝업을 띄운다 (설정 창을 쓸 때. --no-gui 측정과 시험에서는 끈다)
static bool g_overlayInteractive = false;  // 지금 확대 화면 창이 마우스를 받는지 (메뉴나 버튼이 있는 팝업이 떠 있을 때)

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        return g_overlayInteractive ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // 메뉴를 눌러도 게임 창의 포커스를 빼앗지 않는다
    case WM_SETCURSOR:
        if (g_overlayInteractive) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
        if (!g_overlayInteractive) break;
        // 끄는 동안(슬라이더) 창 밖으로 나가도 놓치지 않게 붙잡는다.
        if (msg == WM_LBUTTONDOWN) SetCapture(hwnd);
        else if (msg == WM_LBUTTONUP) ReleaseCapture();
        GuiOverlayMouse(msg, wp, lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateOverlay(const RECT& rc)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OpenScalerOverlay";
        RegisterClassExW(&wc);
        registered = true;
    }

    // LAYERED + TRANSPARENT: 마우스 입력이 아래의 게임 창으로 그대로 전달된다.
    // NOACTIVATE: 오버레이가 포커스를 빼앗지 않는다.
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                L"OpenScalerOverlay", L"OpenScaler", WS_POPUP,
                                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

// 확대 중인 게임의 프로필을 저장한다. 공유할 때 참고하도록 게임 화면 크기도 남긴다.
static void SaveActiveProfile()
{
    if (!g_scaling) return;
    if (g_scaling->srcW > 0) {
        g_scaling->profile.sourceW = (int)g_scaling->srcW;
        g_scaling->profile.sourceH = (int)g_scaling->srcH;
    }
    g_scaling->profile.Save(ProfilesDirectory());
}

// ---------------------------------------------------------------------------
// 프레임 생성 영역 편집 창
// 확대 화면 위에 덮어 지금 영역을 보여 준다. 테두리와 모서리를 끌면 크기가, 안쪽을 끌면 위치가 바뀐다.
// 창 전체를 덮어야 영역 밖 클릭이 게임으로 가지 않으므로, 영역 밖은 어둡게, 안쪽은 거의 투명하게 그린다.
// ---------------------------------------------------------------------------

enum RegionDrag { DRAG_NONE = 0, DRAG_LEFT = 1, DRAG_TOP = 2, DRAG_RIGHT = 4, DRAG_BOTTOM = 8, DRAG_MOVE = 16 };

static struct RegionEditor {
    HWND hwnd = nullptr;
    RECT area{};    // 확대 화면 영역 (화면 좌표). 편집 창이 이만큼 덮는다
    RECT rc{};      // 지금 영역 (창 좌표)
    int drag = DRAG_NONE;
    POINT dragStart{};
    RECT dragStartRc{};
    int minSize = 32;  // 창 좌표의 최소 크기
} g_region;

static const int kRegionGrip = 12;  // 테두리를 잡을 수 있는 두께 (픽셀)

static int RegionHit(POINT p)
{
    const RECT& r = g_region.rc;
    const bool inY = p.y >= r.top - kRegionGrip && p.y <= r.bottom + kRegionGrip;
    const bool inX = p.x >= r.left - kRegionGrip && p.x <= r.right + kRegionGrip;
    int hit = 0;
    if (inY && std::abs(p.x - r.left) <= kRegionGrip) hit |= DRAG_LEFT;
    else if (inY && std::abs(p.x - r.right) <= kRegionGrip) hit |= DRAG_RIGHT;
    if (inX && std::abs(p.y - r.top) <= kRegionGrip) hit |= DRAG_TOP;
    else if (inX && std::abs(p.y - r.bottom) <= kRegionGrip) hit |= DRAG_BOTTOM;
    if (!hit && PtInRect(&r, p)) hit = DRAG_MOVE;
    return hit;
}

// 실행 파일에 넣은 픽셀 글꼴(Neo둥근모 Pro)을 이 프로세스에서만 쓸 수 있게 등록한다 (GDI로 쓰는 안내 문구용).
static bool RegisterPixelFont()
{
    static int state = 0;  // 0: 아직, 1: 됨, 2: 실패
    if (state == 0) {
        state = 2;
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PIXEL_FONT), RT_RCDATA);
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        void* data = mem ? LockResource(mem) : nullptr;
        DWORD count = 0;
        if (data && AddFontMemResourceEx(data, SizeofResource(nullptr, res), nullptr, &count)) state = 1;
    }
    return state == 1;
}

// 반투명 창을 픽셀마다 알파를 정해 다시 그린다 (미리 곱한 알파의 BGRA).
static void RegionPaint()
{
    const int w = g_region.area.right - g_region.area.left, h = g_region.area.bottom - g_region.area.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    auto px = static_cast<uint32_t*>(bits);

    const RECT& r = g_region.rc;
    const uint32_t kDim = 0x78000000;     // 영역 밖: 검정, 알파 120
    const uint32_t kInside = 0x01000000;  // 영역 안: 거의 투명 (알파가 0이면 클릭이 게임으로 간다)
    const uint32_t kSky = 0xFF00AFF0u;    // 테두리: 진한 하늘색 (설정 창의 Scaler 색)
    const uint32_t kSkyLight = 0xFFA6DAF4u;  // 안내 상자 테두리: 연한 하늘색
    const int border = 4;
    for (int y = 0; y < h; y++) {
        uint32_t* row = px + (size_t)y * w;
        const bool rowIn = y >= r.top && y < r.bottom;
        for (int x = 0; x < w; x++) row[x] = rowIn && x >= r.left && x < r.right ? kInside : kDim;
    }
    auto fill = [&](int l, int t, int rr, int b, uint32_t c) {
        l = std::max(l, 0), t = std::max(t, 0), rr = std::min(rr, w), b = std::min(b, h);
        for (int y = t; y < b; y++)
            for (int x = l; x < rr; x++) px[(size_t)y * w + x] = c;
    };
    // 두꺼운 테두리와 손잡이. 영역이 화면 끝에 닿아도 보이도록 영역 안쪽에 그린다.
    fill(r.left, r.top, r.right, r.top + border, kSky);
    fill(r.left, r.bottom - border, r.right, r.bottom, kSky);
    fill(r.left, r.top, r.left + border, r.bottom, kSky);
    fill(r.right - border, r.top, r.right, r.bottom, kSky);
    const int grip = 14;
    const int midX = (r.left + r.right) / 2, midY = (r.top + r.bottom) / 2;
    for (int gy : { (int)r.top + grip / 2, (int)r.bottom - grip / 2, midY })
        for (int gx : { (int)r.left + grip / 2, (int)r.right - grip / 2, midX }) {
            if (gx == midX && gy == midY) continue;
            const int cx = gx, cy = gy;
            fill(cx - grip / 2, cy - grip / 2, cx + grip / 2, cy + grip / 2, 0xFFFFFFFF);
            fill(cx - grip / 2 + 3, cy - grip / 2 + 3, cx + grip / 2 - 3, cy + grip / 2 - 3, kSky);
        }

    // 안내 문구: 흰 상자에 GDI로 쓴 뒤, GDI가 지운 알파를 상자 안에서 다시 채운다.
    const wchar_t* text = L"테두리를 끌어 크기 조절 · 안쪽을 끌어 이동 · Enter 저장 · Esc 취소";
    // 픽셀 글꼴은 16px의 정수 배로만 그린다 (1080p 이상이면 32px). 등록하지 못하면 맑은 고딕을 쓴다.
    const bool pixel = RegisterPixelFont();
    const int fontPx = pixel ? (h >= 1080 ? 32 : 16) : std::max(14, h / 60);
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, pixel ? FW_NORMAL : FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0,
                             pixel ? L"NeoDunggeunmo Pro" : L"Malgun Gothic");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE ts{};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ts);
    RECT box = { (w - ts.cx) / 2 - 12, 16, (w + ts.cx) / 2 + 12, 16 + ts.cy + 12 };
    if (box.bottom + 8 > r.top && box.top < r.bottom && box.right > r.left && box.left < r.right && r.bottom + box.bottom + 24 < h)
        OffsetRect(&box, 0, h - box.bottom - 16);  // 영역과 겹치면 아래로 옮긴다
    fill(box.left - 3, box.top - 3, box.right + 3, box.bottom + 3, kSkyLight);
    fill(box.left, box.top, box.right, box.bottom, 0xFFFFFFFF);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x00, 0xAF, 0xF0));  // 진한 하늘색
    GdiFlush();
    TextOutW(dc, box.left + 12, box.top + 6, text, (int)wcslen(text));
    GdiFlush();
    for (int y = std::max(0L, box.top); y < std::min((LONG)h, box.bottom); y++)
        for (int x = std::max(0L, box.left); x < std::min((LONG)w, box.right); x++) px[(size_t)y * w + x] |= 0xFF000000;
    SelectObject(dc, oldFont);
    DeleteObject(font);

    POINT src = { 0, 0 };
    POINT dst = { g_region.area.left, g_region.area.top };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_region.hwnd, screen, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

static void RegionEditEnd(bool save);

static LRESULT CALLBACK RegionEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        const int hit = g_region.drag ? g_region.drag : RegionHit(p);
        LPCWSTR cursor = IDC_ARROW;
        if (hit == DRAG_MOVE) cursor = IDC_SIZEALL;
        else if (hit == (DRAG_LEFT | DRAG_TOP) || hit == (DRAG_RIGHT | DRAG_BOTTOM)) cursor = IDC_SIZENWSE;
        else if (hit == (DRAG_RIGHT | DRAG_TOP) || hit == (DRAG_LEFT | DRAG_BOTTOM)) cursor = IDC_SIZENESW;
        else if (hit & (DRAG_LEFT | DRAG_RIGHT)) cursor = IDC_SIZEWE;
        else if (hit & (DRAG_TOP | DRAG_BOTTOM)) cursor = IDC_SIZENS;
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        g_region.drag = RegionHit(p);
        if (g_region.drag) {
            g_region.dragStart = p;
            g_region.dragStartRc = g_region.rc;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_region.drag) return 0;
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        const int dx = p.x - g_region.dragStart.x, dy = p.y - g_region.dragStart.y;
        const int w = g_region.area.right - g_region.area.left, h = g_region.area.bottom - g_region.area.top;
        const int m = g_region.minSize;
        RECT r = g_region.dragStartRc;
        if (g_region.drag == DRAG_MOVE) {
            const int ox = std::clamp(dx, (int)-r.left, (int)(w - r.right)), oy = std::clamp(dy, (int)-r.top, (int)(h - r.bottom));
            OffsetRect(&r, ox, oy);
        } else {
            if (g_region.drag & DRAG_LEFT) r.left = std::clamp((int)r.left + dx, 0, (int)r.right - m);
            if (g_region.drag & DRAG_RIGHT) r.right = std::clamp((int)r.right + dx, (int)r.left + m, w);
            if (g_region.drag & DRAG_TOP) r.top = std::clamp((int)r.top + dy, 0, (int)r.bottom - m);
            if (g_region.drag & DRAG_BOTTOM) r.bottom = std::clamp((int)r.bottom + dy, (int)r.top + m, h);
        }
        if (!EqualRect(&r, &g_region.rc)) {
            g_region.rc = r;
            RegionPaint();
        }
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_region.drag && msg == WM_LBUTTONUP) ReleaseCapture();
        g_region.drag = DRAG_NONE;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) RegionEditEnd(true);
        else if (wp == VK_ESCAPE) RegionEditEnd(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static FrameGen::RegionAdvice RegionRecommend(float r[4], double minSeconds = 5.0);
static void RegionEditBegin(const float* initial = nullptr)
{
    if (!g_scaling || !g_scaling->layoutValid || g_region.hwnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = RegionEditorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OpenScalerRegionEditor";
        RegisterClassExW(&wc);
        registered = true;
    }
    const ScalingSession* s = g_scaling;
    g_region.area = s->dstScreen;
    const int w = s->dstScreen.right - s->dstScreen.left, h = s->dstScreen.bottom - s->dstScreen.top;
    // 프레임 생성이 거절하는 크기(원본 64픽셀)보다 작게 줄이지 못하게 한다.
    g_region.minSize = std::max(32, (int)std::ceil(64.0 * w / std::max(1u, s->srcW)));
    const GameProfile& p = s->profile;
    if (initial)
        g_region.rc = { std::lround(initial[0] * w), std::lround(initial[1] * h), std::lround((initial[0] + initial[2]) * w),
                        std::lround((initial[1] + initial[3]) * h) };
    else if (p.HasRegion())
        g_region.rc = { std::lround(p.regionX * w), std::lround(p.regionY * h), std::lround((p.regionX + p.regionW) * w),
                        std::lround((p.regionY + p.regionH) * h) };
    else
        g_region.rc = { w / 4, 0, w - w / 4, h };  // 처음에는 가운데 세로 띠 (리듬게임의 건반 자리)
    g_region.drag = DRAG_NONE;
    g_region.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"OpenScalerRegionEditor", L"OpenScaler 영역 지정",
                                    WS_POPUP, g_region.area.left, g_region.area.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RegionPaint();
    ShowWindow(g_region.hwnd, SW_SHOW);
    SetForegroundWindow(g_region.hwnd);
}

static void RegionEditEnd(bool save)
{
    if (!g_region.hwnd) return;
    if (save && g_scaling) {
        const double w = g_region.area.right - g_region.area.left, h = g_region.area.bottom - g_region.area.top;
        const RECT& r = g_region.rc;
        // 거의 화면 전체를 고르면 영역 없음과 같으므로 지운다.
        if (r.left <= 1 && r.top <= 1 && r.right >= w - 1 && r.bottom >= h - 1) g_scaling->profile.SetRegion(0, 0, 0, 0);
        else g_scaling->profile.SetRegion((float)(r.left / w), (float)(r.top / h), (float)((r.right - r.left) / w), (float)((r.bottom - r.top) / h));
        SaveActiveProfile();
    }
    HWND hwnd = g_region.hwnd;
    g_region.hwnd = nullptr;
    g_region.drag = DRAG_NONE;
    DestroyWindow(hwnd);
}

static void RegionClear()
{
    if (!g_scaling) return;
    g_scaling->profile.SetRegion(0, 0, 0, 0);
    SaveActiveProfile();
}

// ---------------------------------------------------------------------------
// 커서 좌표 변환
// ---------------------------------------------------------------------------

static void ShowSystemCursor(bool show)
{
    if (g_systemCursorHidden != show) return;
    MagShowSystemCursor(show);
    g_systemCursorHidden = !show;
}

static POINT ClampToRect(POINT p, const RECT& r)
{
    return { std::clamp(p.x, r.left, r.right - 1), std::clamp(p.y, r.top, r.bottom - 1) };
}

// 원본 창 위의 점 -> 확대된 화면 위의 점 (픽셀 중심 기준)
static POINT SrcToDst(const ScalingSession* s, POINT p)
{
    const RECT& src = s->srcScreen;
    const RECT& dst = s->dstScreen;
    double sx = double(dst.right - dst.left) / (src.right - src.left);
    double sy = double(dst.bottom - dst.top) / (src.bottom - src.top);
    return { dst.left + (LONG)std::floor((p.x - src.left + 0.5) * sx),
             dst.top + (LONG)std::floor((p.y - src.top + 0.5) * sy) };
}

// 확대된 화면 위의 점 -> 원본 창 위의 점
static POINT DstToSrc(const ScalingSession* s, POINT p)
{
    const RECT& src = s->srcScreen;
    const RECT& dst = s->dstScreen;
    double sx = double(src.right - src.left) / (dst.right - dst.left);
    double sy = double(src.bottom - src.top) / (dst.bottom - dst.top);
    POINT r = { src.left + (LONG)std::floor((p.x - dst.left) * sx),
                src.top + (LONG)std::floor((p.y - dst.top) * sy) };
    return ClampToRect(r, src);
}

static void ReleaseCursor(ScalingSession* s, POINT displayPos)
{
    SetCursorPos(displayPos.x, displayPos.y);
    s->cursorMapped = false;
    ShowSystemCursor(true);
}

// 매 프레임 호출: 커서가 확대 화면에 들어오고 나가는 것을 처리한다.
static void UpdateCursor(ScalingSession* s)
{
    if (!s->layoutValid) return;

    POINT p;
    if (!GetCursorPos(&p)) return;
    // 영역 편집 중이나 확대 화면 위 메뉴가 열려 있으면 그 창을 직접 눌러야 하므로 커서를 원본 창으로 옮기지 않는다.
    if (g_region.hwnd || g_overlayInteractive) {
        if (s->cursorMapped) ReleaseCursor(s, SrcToDst(s, ClampToRect(p, s->srcScreen)));
        return;
    }
    bool foreground = GetForegroundWindow() == s->target;
    const RECT& src = s->srcScreen;
    const RECT& dst = s->dstScreen;

    if (!s->cursorMapped) {
        // 게임 창이 활성 상태이고 커서가 확대된 화면 위에 있으면 원본 창의 대응 위치로 옮긴다.
        if (foreground && PtInRect(&dst, p)) {
            POINT target = DstToSrc(s, p);
            SetCursorPos(target.x, target.y);
            s->cursorMapped = true;
            ShowSystemCursor(false);
        }
        return;
    }

    // 다른 창으로 전환되면 커서를 보이는 위치로 돌려 놓는다.
    if (!foreground) {
        ReleaseCursor(s, SrcToDst(s, ClampToRect(p, src)));
        return;
    }

    if (PtInRect(&src, p)) return;  // 원본 창 안에서 움직이는 중

    // 원본 창 밖으로 나가려 한다: 확대 화면의 같은 가장자리 바로 바깥으로 내보낸다.
    POINT edge = ClampToRect(p, src);
    POINT out = SrcToDst(s, edge);
    if (p.x < src.left) out.x = dst.left - 1;
    else if (p.x >= src.right) out.x = dst.right;
    if (p.y < src.top) out.y = dst.top - 1;
    else if (p.y >= src.bottom) out.y = dst.bottom;

    if (MonitorFromPoint(out, MONITOR_DEFAULTTONULL)) {
        ReleaseCursor(s, out);
    } else {
        // 모니터 끝이라 나갈 곳이 없으면 원본 창 안에 붙잡아 둔다 (전체화면 게임처럼 동작).
        SetCursorPos(edge.x, edge.y);
    }
}

// ---------------------------------------------------------------------------
// 커서 이미지 -> 텍스처
// ---------------------------------------------------------------------------

static std::vector<uint32_t> ReadBitmap(HBITMAP bmp, int w, int h)
{
    std::vector<uint32_t> px((size_t)w * h);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 위에서 아래 순서
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return px;
}

static com_ptr<ID3D11ShaderResourceView> CreatePixelSrv(const std::vector<uint32_t>& px, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { px.data(), w * 4, 0 };
    com_ptr<ID3D11Texture2D> tex;
    check_hresult(g_device->CreateTexture2D(&td, &init, tex.put()));
    com_ptr<ID3D11ShaderResourceView> srv;
    check_hresult(g_device->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
    return srv;
}

// Windows 커서는 세 종류가 있다.
//  - 32비트 알파 컬러 커서: 그대로 사용
//  - 알파 없는 컬러 커서: AND 마스크로 투명도를 정함
//  - 흑백 커서: AND/XOR 마스크 조합 (AND=1, XOR=1이면 배경색 반전, 예: 텍스트 I빔)
static bool LoadCursorImage(HCURSOR hc, CursorImage& img)
{
    ICONINFO ii;
    if (!GetIconInfo(hc, &ii)) return false;

    BITMAP bm = {};
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    const int w = bm.bmWidth;
    const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;  // 흑백 커서는 마스크에 AND/XOR이 위아래로 붙어 있음
    const size_t n = (size_t)w * h;

    std::vector<uint32_t> mask = ReadBitmap(ii.hbmMask, w, ii.hbmColor ? h : h * 2);
    std::vector<uint32_t> color(n, 0), invert(n, 0);
    bool hasInvert = false;

    if (ii.hbmColor) {
        color = ReadBitmap(ii.hbmColor, w, h);
        bool hasAlpha = std::any_of(color.begin(), color.end(), [](uint32_t c) { return (c >> 24) != 0; });
        if (!hasAlpha) {
            for (size_t i = 0; i < n; i++) {
                bool andBit = (mask[i] & 0xFFFFFF) != 0;
                uint32_t rgb = color[i] & 0xFFFFFF;
                if (!andBit) {
                    color[i] = rgb | 0xFF000000;
                } else {
                    color[i] = 0;
                    if (rgb) { invert[i] = rgb | 0xFF000000; hasInvert = true; }
                }
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            bool andBit = (mask[i] & 0xFFFFFF) != 0;
            bool xorBit = (mask[i + n] & 0xFFFFFF) != 0;
            if (!andBit) {
                color[i] = xorBit ? 0xFFFFFFFF : 0xFF000000;
            } else if (xorBit) {
                invert[i] = 0xFFFFFFFF;
                hasInvert = true;
            }
        }
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    img.w = w;
    img.h = h;
    img.hotX = (int)ii.xHotspot;
    img.hotY = (int)ii.yHotspot;
    img.color = CreatePixelSrv(color, w, h);
    if (hasInvert) img.invert = CreatePixelSrv(invert, w, h);
    return true;
}

// ---------------------------------------------------------------------------
// 스케일링 시작 / 종료
// ---------------------------------------------------------------------------

// 메뉴나 버튼이 있는 팝업이 떠 있으면 확대 화면 창이 클릭을 받고, 없으면 다시 통과시킨다.
static void UpdateOverlayInput()
{
    const bool want = g_scaling && g_scaling->overlay && (g_overlayMenu || g_wizard == WIZARD_ASK || g_wizard == WIZARD_EVERYWHERE);
    if (want == g_overlayInteractive) return;
    g_overlayInteractive = want;
    GuiOverlayReset();
    if (!g_scaling || !g_scaling->overlay) return;
    HWND overlay = g_scaling->overlay;
    LONG_PTR ex = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    ex = want ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, ex);
    SetWindowPos(overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (want) {
        ReleaseCapture();
    } else if (GetForegroundWindow() != g_scaling->target) {
        // 메뉴에서 영역 편집 창을 띄웠다면 포커스가 옮겨 갔으므로 게임 창으로 돌려준다 (커서 변환은 게임 창이 활성일 때만 한다).
        SetForegroundWindow(g_scaling->target);
    }
}

static void SetOverlayMenu(bool open)
{
    if (!g_scaling || !g_scaling->overlay) open = false;
    if (g_overlayMenu == open) return;
    g_overlayMenu = open;
    UpdateOverlayInput();
    printf("\n[메뉴] 확대 화면 위 메뉴를 %s\n", open ? "열었습니다" : "닫았습니다");
}

static void SetWizard(int state)
{
    if (g_wizard == state) return;
    g_wizard = state;
    UpdateOverlayInput();
    static const char* const kNames[] = { "끝", "묻는 중", "추천 영역 계산 중", "좁힐 곳 없음" };
    printf("\n[영역 팝업] %s\n", kNames[state]);
}

static void StopScaling()
{
    if (!g_scaling) return;
    SetOverlayMenu(false);
    SetWizard(WIZARD_NONE);
    if (g_scaling->cursorMapped) {
        POINT p;
        GetCursorPos(&p);
        ReleaseCursor(g_scaling, SrcToDst(g_scaling, ClampToRect(p, g_scaling->srcScreen)));
    }
    ShowSystemCursor(true);
    RestoreMouseSpeed();
    SlowMoStop();
    RegionEditEnd(false);
    if (g_frameGen) {
        g_frameGen->SetProfile(nullptr);
        g_frameGen->Reset();
    }
    if (g_scaling->profile.interp.evalCount + g_scaling->profile.extrap.evalCount > 0 || g_scaling->profile.mapSamples > 0 ||
        g_scaling->profile.HasRegion())
        SaveActiveProfile();
    if (g_scaling->session) g_scaling->session.Close();
    if (g_scaling->framePool) g_scaling->framePool.Close();
    g_d2dCtx->SetTarget(nullptr);
    g_ctx->ClearState();
    g_ctx->Flush();
    if (g_scaling->frameWaitable) CloseHandle(g_scaling->frameWaitable);
    if (g_scaling->overlay) DestroyWindow(g_scaling->overlay);
    delete g_scaling;
    g_scaling = nullptr;
    printf("\n[중지] 스케일링을 종료했습니다.\n");
}

// 게임 이름: 창을 만든 프로세스의 실행 파일 이름 (확장자 제외)
static std::wstring GameNameOf(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name = L"unknown";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
            name.assign(path, len);
            size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
            size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) name = name.substr(0, dot);
        }
        CloseHandle(proc);
    }

    // javaw처럼 여러 게임이 함께 쓰는 실행 파일이면 이름만으로는 게임을 구분할 수 없다.
    // 이때는 창 제목에서 게임 이름을 뽑아 붙인다. 예: "Minecraft 26.3 - 싱글플레이" -> "Minecraft (javaw)"
    static const wchar_t* const kSharedHosts[] = { L"javaw", L"java", L"python", L"pythonw", L"node", L"electron", L"love", L"nw" };
    bool shared = false;
    for (const wchar_t* host : kSharedHosts) shared = shared || _wcsicmp(name.c_str(), host) == 0;
    if (!shared) return name;

    wchar_t titleBuf[256] = {};
    GetWindowTextW(hwnd, titleBuf, ARRAYSIZE(titleBuf));
    std::wstring title = titleBuf;
    // " - 싱글플레이"처럼 뒤에 붙는 상태 표시는 바뀔 수 있으므로 버린다.
    for (const wchar_t* sep : { L" - ", L" \x2014 ", L" | " }) {
        size_t at = title.find(sep);
        if (at != std::wstring::npos) title.resize(at);
    }
    // 버전 번호(26.3, v1.2 같은 낱말)는 업데이트마다 바뀌므로 뺀다.
    std::wstring game, word;
    auto flush = [&] {
        bool version = !word.empty();
        for (size_t i = 0; i < word.size(); i++) {
            const wchar_t c = word[i];
            if (!(iswdigit(c) || c == L'.' || (i == 0 && (c == L'v' || c == L'V') && word.size() > 1))) version = false;
        }
        if (!word.empty() && !version) game += (game.empty() ? L"" : L" ") + word;
        word.clear();
    };
    for (wchar_t c : title) {
        if (c == L' ') flush();
        else if (!wcschr(L"\\/:*?\"<>|", c) && c >= 32) word += c;  // 파일 이름에 쓸 수 없는 글자는 뺀다
    }
    flush();
    return game.empty() ? name : game + L" (" + name + L")";
}

static void StartScaling(HWND target)
{
    StopScaling();
    if (!target || !IsWindow(target)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid == GetCurrentProcessId()) return;  // OpenScaler 자신의 창은 확대하지 않는다

    wchar_t title[256] = {};
    GetWindowTextW(target, title, ARRAYSIZE(title));

    auto s = new ScalingSession();
    s->target = target;

    // 게임별 프로필: 실행 파일 이름으로 구분한다.
    s->profile.name = GameNameOf(target);
    s->profile.Load(ProfilePath(ProfilesDirectory(), s->profile.name));
    s->profileSaveTime = Clock::now();

    // 대상 창이 있는 모니터 전체를 덮는다.
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        s->refreshHz = mode.dmDisplayFrequency;
    s->monitor = mi.rcMonitor;
    s->outW = mi.rcMonitor.right - mi.rcMonitor.left;
    s->outH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // 캡처 세션
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    check_hresult(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

    s->poolSize = item.Size();
    s->framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        g_winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, s->poolSize);
    s->session = s->framePool.CreateCaptureSession(item);
    try { s->session.IsCursorCaptureEnabled(false); } catch (...) {}  // 커서는 직접 그린다
    try { s->session.IsBorderRequired(false); } catch (...) {}        // Windows 11 이상에서만 동작

    // 오버레이 창 + 스왑체인
    s->overlay = CreateOverlay(mi.rcMonitor);

    com_ptr<IDXGIFactory2> factory;
    {
        auto dxgiDevice = g_device.as<IDXGIDevice>();
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgiDevice->GetAdapter(adapter.put()));
        check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
    }
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = s->outW;
    scd.Height = s->outH;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    check_hresult(factory->CreateSwapChainForHwnd(g_device.get(), s->overlay, &scd, nullptr, nullptr, s->swapChain.put()));
    factory->MakeWindowAssociation(s->overlay, DXGI_MWA_NO_ALT_ENTER);

    // 기본값으로는 GPU에 최대 3프레임이 대기열로 쌓여 커서와 화면이 늦게 보인다.
    // 대기열을 1프레임으로 줄이고, 표시 직전에 렌더링을 시작하도록 대기 핸들을 쓴다.
    auto swapChain2 = s->swapChain.as<IDXGISwapChain2>();
    check_hresult(swapChain2->SetMaximumFrameLatency(1));
    s->frameWaitable = swapChain2->GetFrameLatencyWaitableObject();

    com_ptr<ID3D11Texture2D> backBuffer;
    check_hresult(s->swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));
    check_hresult(g_device->CreateRenderTargetView(backBuffer.get(), nullptr, s->backBufferRtv.put()));

    // 상태 표시: 글자 크기는 출력 높이에 비례 (1080p에서 24px)
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    check_hresult(g_d2dCtx->CreateBitmapFromDxgiSurface(backBuffer.as<IDXGISurface>().get(), &bp, s->d2dTarget.put()));
    check_hresult(g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, s->outH / 45.0f, L"ko-kr", s->textFormat.put()));
    check_hresult(g_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), s->textBrush.put()));
    check_hresult(g_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), s->bgBrush.put()));
    g_osdUntil = Clock::now() + std::chrono::milliseconds(3000);

    s->session.StartCapture();
    s->statTime = Clock::now();
    g_scaling = s;
    // 모니터가 하나여도 설정을 바꿀 수 있게 메뉴를 열고, 프레임 생성 영역을 지정할지 묻는다.
    if (g_autoOverlay) {
        SetOverlayMenu(true);
        SetWizard(WIZARD_ASK);
    }

    char titleUtf8[768] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, titleUtf8, sizeof(titleUtf8), nullptr, nullptr);
    printf("\n[시작] \"%s\" -> %dx%d 전체화면\n", titleUtf8, s->outW, s->outH);
}

// ---------------------------------------------------------------------------
// 렌더링
// ---------------------------------------------------------------------------

// 대상 창의 클라이언트 영역(제목 표시줄/테두리 제외) 중 모니터에 실제로 보이는 부분을 구한다.
// 창이 화면보다 커서 밖으로 나간 부분은 DWM이 그리지 않아 흰색으로 캡처되므로 잘라낸다.
// box: 캡처 텍스처 안의 위치, screenRect: 화면 좌표
static bool GetVisibleClient(const ScalingSession* s, UINT texW, UINT texH, D3D11_BOX& box, RECT& screenRect)
{
    RECT frame;
    if (FAILED(DwmGetWindowAttribute(s->target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) return false;
    RECT client;
    GetClientRect(s->target, &client);
    POINT tl = { 0, 0 };
    ClientToScreen(s->target, &tl);

    RECT clientScreen = { tl.x, tl.y, tl.x + client.right, tl.y + client.bottom };
    RECT texScreen = { frame.left, frame.top, frame.left + (LONG)texW, frame.top + (LONG)texH };
    RECT visible;
    if (!IntersectRect(&visible, &clientScreen, &s->monitor)) return false;
    if (!IntersectRect(&visible, &visible, &texScreen)) return false;

    box = { (UINT)(visible.left - frame.left), (UINT)(visible.top - frame.top), 0,
            (UINT)(visible.right - frame.left), (UINT)(visible.bottom - frame.top), 1 };
    screenRect = visible;
    return true;
}

// 화면 비율을 유지한 확대 출력 영역을 화면 좌표로 계산한다.
static void UpdateLayout(ScalingSession* s)
{
    if (!s->srcTex) return;

    float scale = std::min((float)s->outW / s->srcW, (float)s->outH / s->srcH);
    if (g_settings.integerScale) scale = std::max(1.0f, std::floor(scale));
    LONG dstW = std::max(1L, std::lround(s->srcW * scale));
    LONG dstH = std::max(1L, std::lround(s->srcH * scale));
    LONG offX = (s->outW - dstW) / 2;
    LONG offY = (s->outH - dstH) / 2;
    s->dstScreen = { s->monitor.left + offX, s->monitor.top + offY,
                     s->monitor.left + offX + dstW, s->monitor.top + offY + dstH };
    s->layoutValid = true;
}

static void EnsureTexture(com_ptr<ID3D11Texture2D>& tex, com_ptr<ID3D11ShaderResourceView>& srv,
                          com_ptr<ID3D11RenderTargetView>* rtv, UINT& curW, UINT& curH, UINT w, UINT h)
{
    if (tex && curW == w && curH == h) return;
    tex = nullptr; srv = nullptr;
    if (rtv) *rtv = nullptr;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    check_hresult(g_device->CreateTexture2D(&td, nullptr, tex.put()));
    check_hresult(g_device->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
    if (rtv) check_hresult(g_device->CreateRenderTargetView(tex.get(), nullptr, rtv->put()));
    curW = w;
    curH = h;
}

static void SetParams(UINT w, UINT h, int mode, float sharpness)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(g_ctx->Map(g_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    auto p = static_cast<ShaderParams*>(mapped.pData);
    p->srcSize[0] = (float)w;
    p->srcSize[1] = (float)h;
    p->invSrcSize[0] = 1.0f / w;
    p->invSrcSize[1] = 1.0f / h;
    p->mode = mode;
    p->sharpness = sharpness;
    g_ctx->Unmap(g_cbuffer.get(), 0);
}

static void DrawPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps,
                     ID3D11ShaderResourceView* srv, UINT srcW, UINT srcH, float sharpness = -1, int mode = -1)
{
    SetParams(srcW, srcH, mode < 0 ? g_settings.mode : mode, sharpness < 0 ? g_settings.sharpness : sharpness);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
}

// 실제 커서 위치(원본 창 안)를 확대 화면 위치로 바꿔 현재 커서 모양을 그린다.
static void DrawCursor(ScalingSession* s)
{
    if (!s->cursorMapped) return;

    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) return;  // 게임이 커서를 숨긴 경우

    auto it = s->cursors.find(ci.hCursor);
    if (it == s->cursors.end()) {
        CursorImage img;
        if (!LoadCursorImage(ci.hCursor, img)) return;
        it = s->cursors.emplace(ci.hCursor, std::move(img)).first;
    }
    const CursorImage& img = it->second;

    POINT pos = SrcToDst(s, ci.ptScreenPos);
    D3D11_VIEWPORT vp = { (float)(pos.x - s->monitor.left - img.hotX), (float)(pos.y - s->monitor.top - img.hotY),
                          (float)img.w, (float)img.h, 0, 1 };

    g_ctx->OMSetBlendState(g_blendAlpha.get(), nullptr, 0xFFFFFFFF);
    DrawPass(s->backBufferRtv.get(), vp, g_psCursor.get(), img.color.get(), img.w, img.h);
    if (img.invert) {
        g_ctx->OMSetBlendState(g_blendInvert.get(), nullptr, 0xFFFFFFFF);
        DrawPass(s->backBufferRtv.get(), vp, g_psCursor.get(), img.invert.get(), img.w, img.h);
    }
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

// 반투명 검은 상자 위에 글자를 그린다. alignBottom이면 y는 상자의 아래쪽 끝이다.
static void DrawLabel(ScalingSession* s, const wchar_t* text, float x, float y, bool alignBottom)
{
    com_ptr<IDWriteTextLayout> layout;
    check_hresult(g_dwFactory->CreateTextLayout(text, (UINT32)wcslen(text), s->textFormat.get(), 4000.0f, 4000.0f,
                                                layout.put()));
    DWRITE_TEXT_METRICS m;
    check_hresult(layout->GetMetrics(&m));
    float pad = s->textFormat->GetFontSize() * 0.5f;
    float h = m.height + pad * 2;
    if (alignBottom) y -= h;
    D2D1_ROUNDED_RECT bg = { { x, y, x + m.width + pad * 2, y + h }, pad * 0.6f, pad * 0.6f };
    g_d2dCtx->FillRoundedRectangle(bg, s->bgBrush.get());
    g_d2dCtx->DrawTextLayout({ x + pad, y + pad }, layout.get(), s->textBrush.get());
}

// 설정을 바꾸면 잠깐(또는 항상) 현재 상태를 보여 주고, 비교 모드에서는 양쪽에 이름표를 붙인다.
static void DrawOsd(ScalingSession* s)
{
    bool showStatus = g_settings.osdAlways || Clock::now() < g_osdUntil;
    const bool slowMoMessage = g_slowMo.state == SLOWMO_OFF && Clock::now() < g_slowMo.messageUntil;
    const bool playing = g_slowMo.state == SLOWMO_PLAYING;
    const bool showSlowMo = slowMoMessage || (g_slowMo.state != SLOWMO_OFF && (!playing || g_slowMo.label));
    if (!showStatus && !g_settings.compare && !showSlowMo) return;

    float left = (float)(s->dstScreen.left - s->monitor.left);
    float top = (float)(s->dstScreen.top - s->monitor.top);
    float right = (float)(s->dstScreen.right - s->monitor.left);
    float bottom = (float)(s->dstScreen.bottom - s->monitor.top);
    float margin = s->textFormat->GetFontSize() * 0.75f;

    g_d2dCtx->SetTarget(s->d2dTarget.get());
    g_d2dCtx->BeginDraw();

    if (showStatus) {
        wchar_t text[384];
        swprintf_s(text, L"%hs  ·  샤픈 %.1f  ·  정수배 %ls  ·  커서 속도 보정 %ls\n%u×%u → %u×%u  ·  출력 %.0f fps  ·  캡처 %.0f fps",
                   kModeNames[g_settings.mode], g_settings.sharpness, g_settings.integerScale ? L"ON" : L"OFF",
                   g_settings.adjustCursorSpeed ? L"ON" : L"OFF",
                   s->srcW, s->srcH, s->midW, s->midH, s->outputFps, s->captureFps);
        if (g_settings.frameGen && g_frameGen) {
            const FgStats& fg = g_frameGen->Stats();
            wchar_t line[160];
            static const wchar_t* const kModeLabels[FG_MODE_COUNT] = { L"블록 매칭", L"RIFE", L"외삽" };
            const wchar_t* method = kModeLabels[FgModeOf(g_settings)];
            if (fg.noRoom)
                swprintf_s(line, L"\n프레임 생성 ON (%ls)  ·  게임이 %.0f fps라 넣을 자리가 없음", method, fg.realFps);
            else if (fg.rifeState == RifeEngine::STATE_READY)
                swprintf_s(line, L"\n프레임 생성 ON (RIFE %dp)  ·  게임 %.0f fps  ·  신경망 %.0f ms  ·  합성 %.1f ms", fg.rifeHeight,
                           fg.realFps, fg.rifeMs, fg.synthMs);
            else
                swprintf_s(line, L"\n프레임 생성 ON (%ls%ls)  ·  게임 %.0f fps  ·  움직임 찾기 %.1f ms  ·  합성 %.1f ms", method,
                           fg.rifeState == RifeEngine::STATE_LOADING ? L", RIFE 준비 중" : fg.rifeState == RifeEngine::STATE_FAILED ? L", RIFE 실패" : L"",
                           fg.realFps, fg.estimateMs, fg.synthMs);
            wcscat_s(text, line);
        }
        DrawLabel(s, text, left + margin, top + margin, false);
    }

    if (showSlowMo) {
        wchar_t text[256];
        const double now = NowSeconds();
        if (slowMoMessage) {
            swprintf_s(text, L"슬로모션 테스트: %ls", g_slowMo.message.c_str());
        } else if (g_slowMo.state == SLOWMO_WAITING) {
            if (!g_frameGen) swprintf_s(text, L"슬로모션 테스트: 프레임 생성기 준비 중");
            else swprintf_s(text, L"슬로모션 테스트: %.0f초 뒤 녹화", std::ceil(std::max(0.0, g_slowMo.startAt - now)));
        } else if (g_slowMo.state == SLOWMO_RECORDING) {
            swprintf_s(text, L"● 녹화 중  %.1f / %d초", std::min(now - g_slowMo.recordStart, (double)g_settings.slowMoSeconds), g_settings.slowMoSeconds);
        } else if (g_slowMo.state == SLOWMO_GENERATING) {
            if (g_frameGen->SlowMoRifeLoading()) swprintf_s(text, L"슬로모션 테스트: RIFE 준비 중");
            else swprintf_s(text, L"프레임 만드는 중  %d / %d", (int)g_frameGen->SlowMoFrames().size(), g_frameGen->SlowMoTotal());
        } else {
            const auto& frames = g_frameGen->SlowMoFrames();
            const auto& f = frames[g_slowMo.index];
            const double slowdown = SlowMoFps(g_settings, s->refreshHz) * g_slowMo.holdVsyncs / s->refreshHz;
            wchar_t kind[64], generator[32];
            if (f.generated) swprintf_s(kind, L"생성 (게임 프레임 %d + %.2f)", f.real + 1, f.t);
            else swprintf_s(kind, L"게임 프레임 %d", f.real + 1);
            if (g_frameGen->SlowMoRifeHeight() > 0) swprintf_s(generator, L"RIFE %dp", g_frameGen->SlowMoRifeHeight());
            else swprintf_s(generator, L"블록 매칭");
            swprintf_s(text, L"슬로모션 %.1f배 느리게  ·  %dHz 가정  ·  %d / %d  ·  %ls  ·  %ls%ls\n"
                             L"Alt+Shift+ Space 멈춤  ·  ←→ 한 장씩  ·  O 글자 끄기  ·  T 끝내기",
                       slowdown, SlowMoFps(g_settings, s->refreshHz), g_slowMo.index + 1, (int)frames.size(), kind, generator, g_slowMo.paused ? L"  ·  멈춤" : L"");
        }
        // 상태 표시가 함께 떠 있으면 그 아래에 놓는다.
        DrawLabel(s, text, left + margin, top + margin + (showStatus ? s->textFormat->GetFontSize() * 6.0f : 0.0f), false);
    }

    if (g_settings.compare) {
        wchar_t text[64];
        const bool generated = g_settings.frameGen || playing;
        swprintf_s(text, L"OpenScaler: %hs%ls", kModeNames[g_settings.mode],
                   !generated ? L"" : FgModeOf(g_settings) == FG_MODE_RIFE ? L" + RIFE" : FgModeOf(g_settings) == FG_MODE_BLOCK ? L" + 블록 매칭" : L" + 외삽");
        DrawLabel(s, generated ? L"일반 확대 (Bilinear), 프레임 생성 끔" : L"일반 확대 (Bilinear)", left + margin, bottom - margin, true);
        DrawLabel(s, text, (left + right) / 2 + margin, bottom - margin, true);
    }

    g_d2dCtx->EndDraw();
    g_d2dCtx->SetTarget(nullptr);
}

static double NowSeconds()
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return double(counter.QuadPart) / double(frequency.QuadPart);
}

static void SlowMoSetPlaybackKeys(bool on)
{
    const UINT mods = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    struct { int id; UINT vk; } keys[] = { { HK_SLOWMO_PAUSE, VK_SPACE }, { HK_SLOWMO_PREV, VK_LEFT }, { HK_SLOWMO_NEXT, VK_RIGHT } };
    for (auto& k : keys) {
        if (on) RegisterHotKey(nullptr, k.id, mods, k.vk);
        else UnregisterHotKey(nullptr, k.id);
    }
}

static void SlowMoStop()
{
    if (g_slowMo.state == SLOWMO_OFF) return;
    if (g_slowMo.state == SLOWMO_PLAYING) SlowMoSetPlaybackKeys(false);
    g_slowMo.state = SLOWMO_OFF;
    if (g_frameGen) {
        g_frameGen->SlowMoClear();
        g_frameGen->SetProfile(nullptr);
        g_frameGen->Reset();
    }
    if (g_scaling) g_scaling->fgWasOn = false;  // 프레임 생성이 켜져 있으면 처음부터 다시 쌓는다
}

static void SlowMoFail(const wchar_t* message)
{
    printf("\n[슬로모션] %s\n", winrt::to_string(message).c_str());
    SlowMoStop();
    g_slowMo.message = message;
    g_slowMo.messageUntil = Clock::now() + std::chrono::seconds(5);
}

static void SlowMoToggle(double delaySeconds)
{
    if (!g_scaling) return;
    if (g_slowMo.state != SLOWMO_OFF) {
        SlowMoStop();
        return;
    }
    g_slowMo.state = SLOWMO_WAITING;
    g_slowMo.startAt = NowSeconds() + delaySeconds;
    g_slowMo.paused = false;
    g_slowMo.label = true;
    g_slowMo.messageUntil = {};
}

static void SlowMoStep(int direction)
{
    if (g_slowMo.state != SLOWMO_PLAYING) return;
    const int count = (int)g_frameGen->SlowMoFrames().size();
    g_slowMo.paused = true;
    g_slowMo.index = (g_slowMo.index + direction + count) % count;
}

// 녹화를 시작할 때가 됐는지 확인하고 프레임을 담을 자리를 잡는다.
static void SlowMoUpdateBeforeCapture(ScalingSession* s)
{
    if (g_slowMo.state != SLOWMO_WAITING || !g_frameGen || !s->srcTex || NowSeconds() < g_slowMo.startAt) return;
    const uint64_t bytes = uint64_t(s->srcW) * s->srcH * 4 * SlowMoFps(g_settings, s->refreshHz) * g_settings.slowMoSeconds;
    if (bytes > kSlowMoMaxBytes) {
        SlowMoFail(L"프레임을 담을 메모리가 모자라요. 녹화 길이나 목표 fps를 줄여 주세요.");
        return;
    }
    try {
        g_frameGen->SlowMoBegin(s->srcTex.get(), s->srcW, s->srcH, (int)std::ceil(g_settings.slowMoSeconds * s->refreshHz) + 4);
    } catch (const winrt::hresult_error&) {
        SlowMoFail(L"프레임을 담을 메모리를 잡지 못했어요.");
        return;
    }
    g_frameGen->SetProfile(&s->profile);
    s->fgWasOn = false;
    g_slowMo.w = s->srcW;
    g_slowMo.h = s->srcH;
    g_slowMo.recordStart = NowSeconds();
    g_slowMo.state = SLOWMO_RECORDING;
}

// --fg-region: 테스트용으로 프로필의 영역 대신 쓴다 (저장하지 않는다). 너비가 0이면 화면 전체, 음수면 쓰지 않음
static float g_fgRegionOverride[4] = { 0, 0, -1, 0 };

static FgOptions CurrentFgOptions(const ScalingSession* s)
{
    FgOptions o;
    o.multiplier = g_settings.fgMultiplier;
    o.quality = g_settings.fgQuality;
    o.method = g_settings.fgMethod;
    o.generator = g_settings.fgGenerator;
    o.visualize = g_settings.fgVisualize && FgModeOf(g_settings) != FG_MODE_RIFE;  // 설정 창에서도 블록 매칭일 때만 보인다
    o.learn = g_settings.fgLearn;
    o.useMap = g_settings.fgPreset != FG_PRESET_NO_MAP;
    o.refreshHz = s->refreshHz;
    float rx = s->profile.regionX, ry = s->profile.regionY, rw = s->profile.regionW, rh = s->profile.regionH;
    if (g_fgRegionOverride[2] >= 0) {
        rx = g_fgRegionOverride[0];
        ry = g_fgRegionOverride[1];
        rw = g_fgRegionOverride[2];
        rh = g_fgRegionOverride[3];
    }
    if (rw > 0 && rh > 0) {
        o.regionX = (int)std::lround(rx * s->srcW);
        o.regionY = (int)std::lround(ry * s->srcH);
        o.regionW = (int)std::lround((rx + rw) * s->srcW) - o.regionX;
        o.regionH = (int)std::lround((ry + rh) * s->srcH) - o.regionY;
    }
    return o;
}

// 캡처 프레임의 보이는 영역을 원본 텍스처로 복사한다. 보이는 영역이 없으면 false.
static bool CopyCaptureFrame(ScalingSession* s, const wgc::Direct3D11CaptureFrame& frame, double& frameTime)
{
    auto contentSize = frame.ContentSize();
    if (contentSize.Width != s->poolSize.Width || contentSize.Height != s->poolSize.Height) {
        // 대상 창 크기가 바뀌면 프레임 풀을 다시 만든다.
        s->poolSize = contentSize;
        s->framePool.Recreate(g_winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> frameTex;
    check_hresult(access->GetInterface(IID_PPV_ARGS(frameTex.put())));

    D3D11_BOX box;
    RECT visible;
    if (!GetVisibleClient(s, contentSize.Width, contentSize.Height, box, visible)) return false;
    EnsureTexture(s->srcTex, s->srcSrv, nullptr, s->srcW, s->srcH, box.right - box.left, box.bottom - box.top);
    g_ctx->CopySubresourceRegion(s->srcTex.get(), 0, 0, 0, 0, frameTex.get(), 0, &box);
    frameTime = frame.SystemRelativeTime().count() / 1e7;  // QPC 기준 캡처 시각 (100ns 단위)
    return true;
}

static void SlowMoRecordCapture(ScalingSession* s, double frameTime)
{
    if (!g_frameGen->SlowMoRecord(s->srcTex.get(), s->srcW, s->srcH, frameTime))
        SlowMoFail(L"녹화 중에 창 크기가 바뀌었어요.");
}

// 확대 중에는 GPU 스케줄링 우선순위를 "실시간"으로 올린다. 화면마다 할 일이 3ms 안팎으로 작은데,
// 게임이 GPU를 많이 쓰는 순간과 겹치면 그 작업이 끝날 때까지 기다려 144Hz의 한 칸(6.9ms)을 넘기고 끊겼다.
// 우선순위를 올리면 우리 작업이 먼저 돌아 DJMAX 144Hz 끊김이 분당 약 66번에서 28번으로 줄었고, 게임 fps는 그대로였다.
// RIFE는 신경망이 GPU를 길게 쓰므로 게임과 바탕화면을 밀어내지 않도록 보통으로 둔다.
static void UpdateGpuPriority()
{
    const bool rife = g_settings.frameGen && FgModeOf(g_settings) == FG_MODE_RIFE;
    // D3DKMT_SCHEDULINGPRIORITYCLASS: 2 보통, 5 실시간
    const int wanted = !g_gpuPriorityAllowed || !g_scaling ? 2 : rife ? g_rifeGpuClass : 5;
    if (wanted == g_gpuPriorityClass) return;
    using SetClass = LONG(APIENTRY*)(HANDLE, int);
    static const auto setClass =
        reinterpret_cast<SetClass>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
    g_gpuPriorityClass = wanted;
    if (setClass && setClass(GetCurrentProcess(), wanted) == 0)
        printf("\n[GPU 우선순위] %d (2 보통, 5 실시간)\n", wanted);
}

static void RenderFrame()
{
    auto s = g_scaling;
    bool newFrame = false;
    double frameTime = 0;

    if (!g_frameGen && g_frameGenLoading.valid() &&
        g_frameGenLoading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        g_frameGen = g_frameGenLoading.get();
    SlowMoUpdateBeforeCapture(s);
    if (g_fgLog) {
        g_frameIndex++;
        for (auto& t : g_frameTimers) {
            if (!t.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            UINT64 t0 = 0, tm = 0, t1 = 0;
            if (g_ctx->GetData(t.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                g_ctx->GetData(t.begin.get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                g_ctx->GetData(t.mid.get(), &tm, sizeof(tm), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                g_ctx->GetData(t.end.get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            t.pending = false;
            if (!dj.Disjoint && dj.Frequency && t.frame > g_timedFrame) {
                g_timedFrame = t.frame;
                g_timedMs = (double)(t1 - t0) * 1000.0 / dj.Frequency;
                g_timedFgMs = (double)(tm - t0) * 1000.0 / dj.Frequency;
            }
        }
        FrameGpuTimer& t = g_frameTimers[g_frameIndex % 8];
        if (!t.disjoint) {
            D3D11_QUERY_DESC dd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, sd = { D3D11_QUERY_TIMESTAMP, 0 };
            check_hresult(g_device->CreateQuery(&dd, t.disjoint.put()));
            check_hresult(g_device->CreateQuery(&sd, t.begin.put()));
            check_hresult(g_device->CreateQuery(&sd, t.mid.put()));
            check_hresult(g_device->CreateQuery(&sd, t.end.put()));
        }
        g_activeTimer = nullptr;
        if (!t.pending) {
            g_ctx->Begin(t.disjoint.get());
            g_ctx->End(t.begin.get());
            t.frame = g_frameIndex;
            g_activeTimer = &t;
            g_timerMidEnded = false;
        }
    }
    if (g_slowMo.state == SLOWMO_RECORDING) g_ctx->ClearState();  // 녹화는 계산 셰이더를 쓰므로 Direct2D가 남긴 상태를 먼저 지운다

    // 1. 가장 최신 캡처 프레임만 사용한다. 녹화 중에는 사이에 온 프레임도 버리지 않는다.
    wgc::Direct3D11CaptureFrame frame{ nullptr };
    while (auto next = s->framePool.TryGetNextFrame()) {
        if (frame && g_slowMo.state == SLOWMO_RECORDING && CopyCaptureFrame(s, frame, frameTime)) SlowMoRecordCapture(s, frameTime);
        frame = next;
        s->capturedFrames++;
    }

    if (frame && CopyCaptureFrame(s, frame, frameTime)) {
        newFrame = true;
        if (g_slowMo.state == SLOWMO_RECORDING) SlowMoRecordCapture(s, frameTime);
    }

    if (g_slowMo.state == SLOWMO_RECORDING &&
        (g_frameGen->SlowMoSpan() >= g_settings.slowMoSeconds || g_frameGen->SlowMoFull() ||
         NowSeconds() - g_slowMo.recordStart > g_settings.slowMoSeconds + 1.5)) {
        if (g_frameGen->SlowMoStartGenerating(SlowMoFps(g_settings, s->refreshHz), SlowMoFps(g_settings, s->refreshHz) * g_settings.slowMoSeconds))
            g_slowMo.state = SLOWMO_GENERATING;
        else SlowMoFail(L"화면이 움직이지 않아 만들 프레임이 없어요.");
    }

    // 2. 원본 영역(창이 움직였을 수 있으므로 매 프레임), 출력 영역, 커서 위치 처리
    D3D11_BOX box;
    RECT visible;
    if (s->srcTex && GetVisibleClient(s, s->poolSize.Width, s->poolSize.Height, box, visible))
        s->srcScreen = { visible.left, visible.top, visible.left + (LONG)s->srcW, visible.top + (LONG)s->srcH };
    UpdateLayout(s);
    UpdateCursor(s);

    // 커서가 확대 화면 안에 있을 때만 마우스 속도를 낮춘다.
    if (g_settings.adjustCursorSpeed && s->cursorMapped && s->layoutValid)
        ApplyMouseSpeed(float(s->dstScreen.right - s->dstScreen.left) / (s->srcScreen.right - s->srcScreen.left));
    else
        RestoreMouseSpeed();

    // 지난 프레임의 Direct2D 그리기가 남긴 파이프라인 상태를 지우고 시작한다.
    g_ctx->ClearState();
    float clear[4] = { 0, 0, 0, 1 };
    g_ctx->ClearRenderTargetView(s->backBufferRtv.get(), clear);

    if (s->layoutValid) {
        UINT dstW = s->dstScreen.right - s->dstScreen.left;
        UINT dstH = s->dstScreen.bottom - s->dstScreen.top;
        float offX = (float)(s->dstScreen.left - s->monitor.left);
        float offY = (float)(s->dstScreen.top - s->monitor.top);

        EnsureTexture(s->midTex, s->midSrv, &s->midRtv, s->midW, s->midH, dstW, dstH);

        // 프레임 생성: 켜져 있으면 업스케일에 들어갈 영상을 합성 결과로 바꾼다.
        ID3D11ShaderResourceView* displaySrv = s->srcSrv.get();
        ID3D11ShaderResourceView* compareSrv = displaySrv;
        UINT contentW = s->srcW, contentH = s->srcH;
        if (g_slowMo.state == SLOWMO_GENERATING) {
            FgOptions options = CurrentFgOptions(s);
            options.learn = false;
            try {
                if (g_frameGen->SlowMoGenerateStep(options)) {
                    if (!g_slowMo.dumpDir.empty()) {
                        CreateDirectoryW(g_slowMo.dumpDir.c_str(), nullptr);
                        const auto& frames = g_frameGen->SlowMoFrames();
                        for (int i = 0; i < (int)frames.size(); i++) {
                            wchar_t name[64];
                            swprintf_s(name, L"\\frame_%03d_%ls.bmp", i, frames[i].generated ? L"gen" : L"real");
                            SaveFrameBmp(g_slowMo.dumpDir + name, g_frameGen->SlowMoReadFrame(i), g_slowMo.w, g_slowMo.h);
                        }
                        // 게임 프레임의 캡처 시각: --fg-gaptest가 녹화의 실제 프레임 간격을 알 수 있게 한다.
                        FILE* times = nullptr;
                        if (_wfopen_s(&times, (g_slowMo.dumpDir + L"\\real_times.txt").c_str(), L"w") == 0 && times) {
                            for (int i = 0; i < g_frameGen->SlowMoRealCount(); i++)
                                fprintf(times, "%.6f\n", g_frameGen->SlowMoRealTime(i) - g_frameGen->SlowMoRealTime(0));
                            fclose(times);
                        }
                    }
                    g_slowMo.index = g_slowMo.vsyncs = g_slowMo.loopWait = 0;
                    // 주사율이 목표 fps보다 높으면 한 장을 더 오래 보여 줘 실제 시간보다 빨라지지 않게 한다.
                    g_slowMo.holdVsyncs = std::max(1, (int)std::ceil(s->refreshHz / SlowMoFps(g_settings, s->refreshHz) - 0.01));
                    g_slowMo.state = SLOWMO_PLAYING;
                    SlowMoSetPlaybackKeys(true);
                    printf("\n[슬로모션] 게임 프레임 %d장으로 %d장을 만들었습니다.\n", g_frameGen->SlowMoRealCount(), (int)g_frameGen->SlowMoFrames().size());
                }
            } catch (const winrt::hresult_error&) {
                SlowMoFail(L"프레임을 담을 메모리를 잡지 못했어요.");
            }
        }
        if (g_slowMo.state == SLOWMO_PLAYING) {
            const auto& f = g_frameGen->SlowMoFrames()[g_slowMo.index];
            displaySrv = f.srv;
            compareSrv = f.held;
            contentW = g_slowMo.w;
            contentH = g_slowMo.h;
            if (g_fgLog) fprintf(g_fgLog, "slowmo,%.6f,%d,%d,%.3f,%d\n", NowSeconds(), g_slowMo.index, f.real, f.t, f.generated ? 1 : 0);
        } else if (g_slowMo.state != SLOWMO_OFF) {
            // 녹화와 생성 중에는 지금 화면을 보간 없이 보여 준다.
        } else if (g_settings.frameGen && g_frameGen) {
            if (!s->fgWasOn) {
                g_frameGen->Reset();
                g_frameGen->SetProfile(&s->profile);
                s->fgWasOn = true;
                newFrame = true;  // 켜자마자 지금 영상부터 쌓는다
                frameTime = NowSeconds();
            }
            FgOptions options = CurrentFgOptions(s);
            // CPU가 프레임 생성에 머문 시간 (GPU 결과를 기다리느라 멈추면 여기서 길어진다)
            const double pushStart = NowSeconds();
            if (newFrame) g_frameGen->PushFrame(s->srcTex.get(), s->srcW, s->srcH, frameTime, options);
            double now = NowSeconds();
            const double pushMs = (now - pushStart) * 1000.0;
            if (auto generated = g_frameGen->Render(now, options)) {
                displaySrv = generated;
                if (auto held = g_frameGen->HeldFrame()) compareSrv = held;
            }
            const double renderMs = (NowSeconds() - now) * 1000.0;
            if (g_activeTimer && !g_timerMidEnded) {
                g_ctx->End(g_activeTimer->mid.get());
                g_timerMidEnded = true;
            }
            if (g_fgLog) {
                const FgStats& st = g_frameGen->Stats();
                fprintf(g_fgLog, "%.6f,%.4f,%lld,%lld,%lld,%lld,%.1f,%.6f,%d,%.3f,%.3f,%d,%d,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%lld,%lld,%.3f,%.3f\n", now, st.displayT, st.realFrames,
                        st.duplicates, st.rifeShown, st.rifeMissed, st.rifeMs, st.contentTime, st.rifeHeight, st.estimateMs, st.synthMs, st.workW,
                        st.workH, pushMs, renderMs, st.acceptLagMs, st.rifeLagMs, st.rifeLagLastMs,
                        newFrame ? (pushStart - frameTime) * 1000.0 : -1.0, g_frameIndex, g_timedFrame, g_timedMs, g_timedFgMs);
            }
        } else if (s->fgWasOn && !g_settings.frameGen) {
            g_frameGen->SetProfile(nullptr);
            g_frameGen->Reset();
            s->fgWasOn = false;
        }

        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_vs.get(), nullptr, 0);
        ID3D11SamplerState* sampler = g_sampler.get();
        g_ctx->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* cbs[2] = { g_cbuffer.get(), g_fsrCbuffer.get() };
        g_ctx->PSSetConstantBuffers(0, 2, cbs);

        const bool fsr = g_settings.mode == MODE_FSR;
        if (fsr) {
            // 샤프닝 1.0 -> RCAS 0스톱(최대), 0.5 -> 1스톱, 0.0에 가까울수록 약하게
            FsrParams fp = {};
            ComputeFsrParams(fp, (float)contentW, (float)contentH, (float)dstW, (float)dstH,
                             (1.0f - g_settings.sharpness) * 2.0f);
            fp.outputOffset[0] = (int32_t)offX;
            fp.outputOffset[1] = (int32_t)offY;
            D3D11_MAPPED_SUBRESOURCE mapped;
            check_hresult(g_ctx->Map(g_fsrCbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &fp, sizeof(fp));
            g_ctx->Unmap(g_fsrCbuffer.get(), 0);
        }

        // Pass 1: 업스케일 (FSR 모드는 EASU)
        D3D11_VIEWPORT vp1 = { 0, 0, (float)dstW, (float)dstH, 0, 1 };
        DrawPass(s->midRtv.get(), vp1, fsr ? g_psEasu.get() : g_psScale.get(), displaySrv, contentW, contentH);

        // Pass 2: 샤프닝하면서 백버퍼 중앙에 출력 (FSR 모드는 RCAS, 샤프닝 0이면 그대로 복사)
        D3D11_VIEWPORT vp2 = { offX, offY, (float)dstW, (float)dstH, 0, 1 };
        if (fsr && g_settings.sharpness > 0)
            DrawPass(s->backBufferRtv.get(), vp2, g_psRcas.get(), s->midSrv.get(), dstW, dstH);
        else
            DrawPass(s->backBufferRtv.get(), vp2, g_psSharpen.get(), s->midSrv.get(), dstW, dstH, fsr ? 0.0f : -1.0f);

        // 비교 모드: 왼쪽 절반을 Bilinear(샤프닝 없음)로 덮어 그리고 가운데에 구분선을 긋는다.
        if (g_settings.compare) {
            LONG half = (LONG)offX + (LONG)dstW / 2;
            D3D11_RECT left = { (LONG)offX, (LONG)offY, half, (LONG)offY + (LONG)dstH };
            g_ctx->RSSetState(g_rsScissor.get());
            g_ctx->RSSetScissorRects(1, &left);
            DrawPass(s->backBufferRtv.get(), vp2, g_psScale.get(), compareSrv, contentW, contentH, 0.0f, MODE_BILINEAR);
            g_ctx->RSSetState(nullptr);

            D3D11_VIEWPORT line = { (float)(half - 1), offY, 2, (float)dstH, 0, 1 };
            DrawPass(s->backBufferRtv.get(), line, g_psCursor.get(), g_whiteSrv.get(), 1, 1);
        }

        // Pass 3: 커서
        DrawCursor(s);

        // Pass 4: 화면 위 상태 표시
        DrawOsd(s);

        // Pass 5: 확대 화면 위 메뉴와 팝업. 영역 편집 창을 띄운 동안에는 숨겨 편집 창을 가리지 않는다.
        if ((g_overlayMenu || g_wizard != WIZARD_NONE) && !g_region.hwnd) {
            OverlayView view;
            view.menu = g_overlayMenu;
            view.needSeconds = kWizardSeconds;
            if (g_wizard == WIZARD_COMPUTING) {
                // 추천 영역이 나오면 편집 창을 띄워 사용자가 다듬게 한다.
                float r[4];
                const int advice = (int)RegionRecommend(r, kWizardSeconds);
                view.seconds = g_frameGen ? g_frameGen->ChangeSeconds() : 0;
                view.still = advice == FrameGen::REGION_STILL;
                if (advice == FrameGen::REGION_OK) {
                    SetWizard(WIZARD_NONE);
                    printf("\n[영역 팝업] 추천 영역 %.3f,%.3f,%.3f,%.3f\n", r[0], r[1], r[2], r[3]);
                    RegionEditBegin(r);
                } else if (advice == FrameGen::REGION_EVERYWHERE) {
                    SetWizard(WIZARD_EVERYWHERE);
                }
            }
            static const int kPrompt[] = { OVERLAY_PROMPT_NONE, OVERLAY_PROMPT_ASK_REGION, OVERLAY_PROMPT_COMPUTING, OVERLAY_PROMPT_EVERYWHERE };
            view.prompt = kPrompt[g_wizard];
            if (!g_region.hwnd && (view.menu || view.prompt != OVERLAY_PROMPT_NONE)) {
                switch (GuiOverlayRender(g_device.get(), g_ctx.get(), s->backBufferRtv.get(), (int)s->outW, (int)s->outH, view)) {
                case OVERLAY_ACTION_CLOSE_MENU: SetOverlayMenu(false); break;
                case OVERLAY_ACTION_YES:
                    // 영역은 프레임 생성에만 쓰이므로 꺼져 있으면 켠다. 누른 뒤의 화면만 보도록 모은 변화를 버린다.
                    if (!g_settings.frameGen) {
                        g_settings.frameGen = true;
                        AppSettingsChanged();
                    }
                    if (g_frameGen) g_frameGen->ResetChange();
                    g_overlayMenu = false;  // 메뉴를 숨겨 게임 화면이 보이고 마우스도 게임으로 가게 한다
                    SetWizard(WIZARD_COMPUTING);
                    break;
                case OVERLAY_ACTION_NO: SetWizard(WIZARD_NONE); break;
                case OVERLAY_ACTION_MANUAL:
                    SetWizard(WIZARD_NONE);
                    RegionEditBegin();
                    break;
                case OVERLAY_ACTION_DISMISS: SetWizard(WIZARD_NONE); break;
                }
            }
        }

        // 슬로모션 재생은 시간이 아니라 화면을 그린 횟수로 넘긴다. 그려야 할 때를 놓쳐도 건너뛰는 프레임이 없다.
        if (g_slowMo.state == SLOWMO_PLAYING && !g_slowMo.paused && ++g_slowMo.vsyncs >= g_slowMo.holdVsyncs) {
            g_slowMo.vsyncs = 0;
            if (g_slowMo.index + 1 < (int)g_frameGen->SlowMoFrames().size()) g_slowMo.index++;
            else if (++g_slowMo.loopWait >= kSlowMoLoopWait) g_slowMo.index = g_slowMo.loopWait = 0;
        }
    }

    if (g_activeTimer) {
        if (!g_timerMidEnded) g_ctx->End(g_activeTimer->mid.get());
        g_ctx->End(g_activeTimer->end.get());
        g_ctx->End(g_activeTimer->disjoint.get());
        g_activeTimer->pending = true;
        g_activeTimer = nullptr;
    }
    // 3. 수직동기에 맞춰 출력
    s->swapChain->Present(1, 0);
    s->presentedFrames++;
}

// ---------------------------------------------------------------------------
// 상태 표시 / 단축키
// ---------------------------------------------------------------------------

static void PrintStatus()
{
    if (!g_scaling) return;
    auto now = Clock::now();
    double sec = std::chrono::duration<double>(now - g_scaling->statTime).count();
    if (sec < 1.0) return;

    int currentMouseSpeed = 0;
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &currentMouseSpeed, 0);
    printf("\r  %ux%u -> %ux%u | 모드: %-8s | 샤픈: %.1f | 정수배: %-3s | 비교: %-3s | 커서: %-4s | 마우스 속도: %2d | 캡처 %3.0f fps | 출력 %3.0f fps   ",
           g_scaling->srcW, g_scaling->srcH, g_scaling->midW, g_scaling->midH,
           kModeNames[g_settings.mode], g_settings.sharpness, g_settings.integerScale ? "ON" : "OFF",
           g_settings.compare ? "ON" : "OFF",
           g_scaling->cursorMapped ? "변환" : "일반",
           currentMouseSpeed,
           g_scaling->capturedFrames / sec, g_scaling->presentedFrames / sec);
    fflush(stdout);
    g_scaling->captureFps = g_scaling->capturedFrames / sec;
    g_scaling->outputFps = g_scaling->presentedFrames / sec;
    g_scaling->capturedFrames = 0;
    g_scaling->presentedFrames = 0;
    g_scaling->statTime = now;

    // 비정상 종료에 대비해 학습 결과를 가끔 저장한다.
    if (now - g_scaling->profileSaveTime > std::chrono::seconds(60)) {
        g_scaling->profileSaveTime = now;
        if (g_scaling->profile.interp.evalCount + g_scaling->profile.extrap.evalCount > 0 || g_scaling->profile.mapSamples > 0 ||
        g_scaling->profile.HasRegion())
            SaveActiveProfile();
    }
}

static bool HandleHotkey(int id)
{
    switch (id) {
    case HK_TOGGLE:
        if (g_scaling) StopScaling();
        else {
            // 설정 창이 맨 앞에 있으면 설정 창에서 고른 창을 확대한다.
            HWND target = GetForegroundWindow();
            if (target == GuiHwnd()) target = GuiSelectedWindow();
            StartScaling(target);
        }
        break;
    case HK_MODE:
        g_settings.mode = (g_settings.mode + 1) % MODE_COUNT;
        break;
    case HK_INTEGER:
        g_settings.integerScale = !g_settings.integerScale;
        break;
    case HK_COMPARE:
        g_settings.compare = !g_settings.compare;
        break;
    case HK_OSD:
        // 슬로모션 재생 중에는 프레임 글자를 끄고 켠다. 글자 없이 화면만 볼 수 있게 한다.
        if (g_slowMo.state == SLOWMO_PLAYING) {
            g_slowMo.label = !g_slowMo.label;
            return true;
        }
        g_settings.osdAlways = !g_settings.osdAlways;
        break;
    case HK_CURSOR_SPEED:
        g_settings.adjustCursorSpeed = !g_settings.adjustCursorSpeed;
        break;
    case HK_FRAMEGEN:
        g_settings.frameGen = !g_settings.frameGen;
        break;
    case HK_SLOWMO:
        SlowMoToggle(0);
        return true;
    case HK_SLOWMO_PAUSE:
        g_slowMo.paused = !g_slowMo.paused;
        return true;
    case HK_SLOWMO_PREV:
        SlowMoStep(-1);
        return true;
    case HK_SLOWMO_NEXT:
        SlowMoStep(1);
        return true;
    case HK_SHARP_UP:
        g_settings.sharpness = std::min(1.0f, g_settings.sharpness + 0.1f);
        break;
    case HK_SHARP_DOWN:
        g_settings.sharpness = std::max(0.0f, g_settings.sharpness - 0.1f);
        break;
    case HK_MENU:
        if (g_wizard == WIZARD_COMPUTING) {
            SetWizard(WIZARD_NONE);  // 추천 영역 계산을 그만두고 메뉴를 연다
            SetOverlayMenu(true);
        } else {
            SetOverlayMenu(!g_overlayMenu);
        }
        return true;
    case HK_QUIT:
        return false;
    }
    if (id != HK_TOGGLE) AppSettingsChanged();
    return true;
}

// ---------------------------------------------------------------------------
// 설정 창(gui.cpp)에서 쓰는 기능 (app.h)
// ---------------------------------------------------------------------------

Settings& AppSettings() { return g_settings; }

void AppSettingsChanged()
{
    g_osdUntil = Clock::now() + std::chrono::milliseconds(2500);  // 바뀐 설정을 화면에 잠깐 보여 준다
    SaveSettings();
}

void AppStartScaling(HWND target) { StartScaling(target); }
void AppStopScaling() { StopScaling(); }
void AppSlowMoToggle(double delaySeconds) { SlowMoToggle(delaySeconds); }
void AppRegionEditBegin() { RegionEditBegin(); }
void AppRegionEditEnd(bool save) { RegionEditEnd(save); }
bool AppRegionEditing() { return g_region.hwnd != nullptr; }
void AppRegionClear() { RegionClear(); }

static FrameGen::RegionAdvice RegionRecommend(float r[4], double minSeconds)
{
    if (!g_scaling || !g_settings.frameGen) return (FrameGen::RegionAdvice)-1;
    // 프레임 생성이 켜져 있지만 아직 준비 중이면 모으는 중으로 본다.
    if (!g_frameGen || !g_scaling->fgWasOn) return FrameGen::REGION_NOT_ENOUGH;
    return g_frameGen->RecommendRegion(r, minSeconds);
}

int AppRegionRecommend()
{
    float r[4];
    const int advice = (int)RegionRecommend(r);
    if (advice == FrameGen::REGION_OK) RegionEditBegin(r);
    return advice;
}

// 공유받은 학습 데이터를 profiles 폴더에 넣는다. 같은 게임의 기존 파일은 profiles/backup으로 옮겨 둔다.
bool AppImportProfile(const std::wstring& file, std::wstring& gameName, std::wstring& message)
{
    GameProfile incoming;
    if (!incoming.Load(file)) {
        message = L"OpenScaler 학습 데이터 파일이 아니에요.";
        return false;
    }
    // 파일에 적힌 게임 이름을 쓴다. 예전 파일처럼 이름이 없으면 파일 이름을 쓴다.
    gameName = GameProfile::ReadName(file);
    if (gameName.empty()) {
        gameName = file.substr(file.find_last_of(L"\\/") + 1);
        if (gameName.size() > 5 && _wcsicmp(gameName.c_str() + gameName.size() - 5, L".json") == 0) gameName.resize(gameName.size() - 5);
    }
    const std::wstring dir = ProfilesDirectory();
    const std::wstring dest = ProfilePath(dir, gameName);
    wchar_t fullFile[MAX_PATH], fullDest[MAX_PATH];
    if (GetFullPathNameW(file.c_str(), MAX_PATH, fullFile, nullptr) && GetFullPathNameW(dest.c_str(), MAX_PATH, fullDest, nullptr) &&
        _wcsicmp(fullFile, fullDest) == 0) {
        message = L"이미 profiles 폴더에 있는 파일이에요.";
        return false;
    }

    // 확대 중인 게임이면 지금까지의 학습을 먼저 저장해서 함께 백업한다.
    const bool active = g_scaling && g_scaling->profile.name == gameName;
    if (active) SaveActiveProfile();
    CreateDirectoryW(dir.c_str(), nullptr);
    const bool hadData = GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hadData) {
        const std::wstring backupDir = dir + L"\\backup";
        CreateDirectoryW(backupDir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf_s(stamp, L"_%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring backup = ProfilePath(backupDir, gameName);
        backup.insert(backup.size() - 5, stamp);
        if (!MoveFileExW(dest.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            message = L"기존 데이터를 백업하지 못해 가져오지 않았어요.";
            return false;
        }
    }
    // 이름을 고쳐 다시 저장한다 (파일 이름과 안에 적힌 이름을 맞춘다).
    incoming.name = gameName;
    if (!incoming.Save(dir)) {
        message = L"profiles 폴더에 저장하지 못했어요.";
        return false;
    }
    if (active) {
        // 확대 중인 게임이면 바로 공유받은 데이터로 바꾸고, 그 위에서 이어서 학습한다.
        g_scaling->profile = incoming;
        if (g_frameGen) g_frameGen->ProfileReloaded();
    }
    message = hadData ? L"가져왔어요. 이전 데이터는 profiles\\backup에 있어요." : L"가져왔어요.";
    return true;
}

// 학습 데이터를 공유용으로 내보낸다. 확대 중인 게임이면 저장 전 기록까지 담는다.
bool AppExportProfile(const std::wstring& gameName, const std::wstring& file)
{
    if (g_scaling && g_scaling->profile.name == gameName) {
        SaveActiveProfile();
        return g_scaling->profile.SaveTo(file);
    }
    GameProfile p;
    p.name = gameName;
    return p.Load(ProfilePath(ProfilesDirectory(), gameName)) && p.SaveTo(file);
}
const std::wstring& AppSettingsPath() { return g_settingsPath; }

ScalingStatus AppGetStatus()
{
    ScalingStatus st;
    if (!g_scaling) return st;
    st.active = true;
    st.target = g_scaling->target;
    st.srcW = g_scaling->srcW;
    st.srcH = g_scaling->srcH;
    st.dstW = g_scaling->midW;
    st.dstH = g_scaling->midH;
    st.outputFps = g_scaling->outputFps;
    st.captureFps = g_scaling->captureFps;
    if (g_settings.frameGen && g_frameGen) {
        const FgStats& fg = g_frameGen->Stats();
        st.fgActive = true;
        st.fgNoRoom = fg.noRoom;
        st.fgRealFps = fg.realFps;
        st.fgEstimateMs = fg.estimateMs;
        st.fgSynthMs = fg.synthMs;
        st.fgRifeState = fg.rifeState;
        st.fgRifeMs = fg.rifeMs;
    }
    st.refreshHz = g_scaling->refreshHz;
    st.slowMoState = g_slowMo.state;
    if (g_frameGen && g_slowMo.state == SLOWMO_GENERATING) {
        st.slowMoDone = (int)g_frameGen->SlowMoFrames().size();
        st.slowMoTotal = g_frameGen->SlowMoTotal();
    } else if (g_frameGen && g_slowMo.state == SLOWMO_PLAYING) {
        st.slowMoDone = g_slowMo.index + 1;
        st.slowMoTotal = (int)g_frameGen->SlowMoFrames().size();
    }
    return st;
}

const GameProfile* AppActiveProfile() { return g_scaling ? &g_scaling->profile : nullptr; }

static void RegisterHotkeys()
{
    const UINT mods = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    struct { int id; UINT vk; const char* name; } keys[] = {
        { HK_TOGGLE, 'S', "Alt+Shift+S" },        { HK_MODE, 'M', "Alt+Shift+M" },
        { HK_INTEGER, 'I', "Alt+Shift+I" },       { HK_SHARP_UP, VK_UP, "Alt+Shift+Up" },
        { HK_SHARP_DOWN, VK_DOWN, "Alt+Shift+Down" }, { HK_COMPARE, 'C', "Alt+Shift+C" },
        { HK_OSD, 'O', "Alt+Shift+O" },           { HK_CURSOR_SPEED, 'P', "Alt+Shift+P" },
        { HK_FRAMEGEN, 'F', "Alt+Shift+F" },      { HK_SLOWMO, 'T', "Alt+Shift+T" },
        { HK_QUIT, 'Q', "Alt+Shift+Q" },          { HK_MENU, 'G', "Alt+Shift+G" },
    };
    for (auto& k : keys) {
        if (!RegisterHotKey(nullptr, k.id, mods, k.vk))
            printf("경고: 단축키 %s 등록 실패 (다른 프로그램이 사용 중일 수 있음)\n", k.name);
    }
}

// 제목에 문자열이 포함된 보이는 창을 찾는다 (테스트/자동화용).
static HWND FindWindowByTitle(const std::wstring& needle)
{
    struct Ctx { const std::wstring* needle; HWND found; } ctx = { &needle, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(hwnd) || hwnd == GetConsoleWindow()) return TRUE;
        wchar_t title[256];
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        if (wcsstr(title, c->needle->c_str())) { c->found = hwnd; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 콘솔 창을 닫거나 Ctrl+C로 끝낼 때도 숨긴 시스템 커서를 되살린다.
static BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    if (g_systemCursorHidden) MagShowSystemCursor(TRUE);
    RestoreMouseSpeed();
    return FALSE;
}

static void PrintHelp()
{
    printf("OpenScaler\n"
           "------------------------------------------------------------\n"
           "  Alt+Shift+S     현재 활성 창 스케일링 시작/중지\n"
           "  Alt+Shift+M     업스케일 모드 변경 (Nearest/Bilinear/Bicubic/Lanczos2/FSR 1.0)\n"
           "  Alt+Shift+I     정수배 스케일링 켜기/끄기\n"
           "  Alt+Shift+C     비교 모드 (왼쪽: 일반 확대, 오른쪽: 현재 모드)\n"
           "  Alt+Shift+O     화면 위 상태 표시 항상 켜기/끄기\n"
           "  Alt+Shift+P     커서 속도 보정 켜기/끄기 (확대 중 마우스 속도를 배율만큼 낮춤)\n"
           "  Alt+Shift+F     프레임 생성 켜기/끄기\n"
           "  Alt+Shift+T     슬로모션 테스트 시작/끝내기 (녹화 -> 프레임 생성 -> 느리게 재생)\n"
           "  Alt+Shift+Up    샤프닝 강하게\n"
           "  Alt+Shift+Down  샤프닝 약하게\n"
           "  Alt+Shift+Q     프로그램 종료\n"
           "------------------------------------------------------------\n"
           "게임을 창 모드로 실행하고, 게임 창을 클릭한 뒤 Alt+Shift+S를 누르세요.\n\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // 저장된 설정을 먼저 불러오고, 명령줄 옵션이 있으면 그 값으로 덮어쓴다.
    g_settingsPath = GetSettingsPath();
    LoadSettings();
    RecoverMouseSpeed();

    // 명령줄 옵션: --target "창 제목 일부" --duration 초 --mode 0~4 --sharp 0~1 --integer
    //             --no-gui (설정 창 없이 실행) --console (로그를 보여 줄 콘솔 창 열기)
    std::wstring targetTitle;
    std::wstring fgRegionTestDir;
    std::wstring regionBenchDump, regionBenchOut;
    float regionBench[4] = { 0.25f, 0.0f, 0.5f, 1.0f };
    std::wstring importProfile, exportProfileName, exportProfileFile;  // 학습 데이터 공유 (설정 창 버튼과 같다)
    std::wstring fgSelfTestDir, rifeBenchDir, fgGapTestDump, fgGapTestOut, fgGapTestProfile;
    double duration = 0;
    double slowMoAuto = -1;
    bool regionEditAuto = false;  // 테스트용: 확대가 시작되면 영역 편집 창을 띄운다
    double regionRecommendAt = -1;  // 테스트용: 확대 후 이 초가 지나면 추천 영역을 출력한다 (편집 창은 띄우지 않는다)
    double scalingSince = -1;
    bool overlayMenuAuto = false;  // 테스트용: 확대가 시작되면 확대 화면 위 메뉴를 연다 (게임에 단축키를 보내지 않고 시험)
    bool useGui = true;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--target" && i + 1 < argc) targetTitle = argv[++i];
        else if (a == L"--no-gui") useGui = false;
        else if (a == L"--console") {
            // GUI 프로그램이라 기본으로는 콘솔이 없다. 디버깅할 때만 따로 연다.
            AllocConsole();
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            SetConsoleOutputCP(CP_UTF8);
        }
        else if (a == L"--duration" && i + 1 < argc) duration = _wtof(argv[++i]);
        else if (a == L"--mode" && i + 1 < argc) g_settings.mode = std::clamp(_wtoi(argv[++i]), 0, MODE_COUNT - 1);
        else if (a == L"--sharp" && i + 1 < argc) g_settings.sharpness = std::clamp((float)_wtof(argv[++i]), 0.0f, 1.0f);
        else if (a == L"--integer") g_settings.integerScale = true;
        else if (a == L"--compare") g_settings.compare = true;
        else if (a == L"--cursor-speed") g_settings.adjustCursorSpeed = true;
        else if (a == L"--fg-selftest" && i + 1 < argc) fgSelfTestDir = argv[++i];
        else if (a == L"--fg-regiontest" && i + 1 < argc) fgRegionTestDir = argv[++i];
        else if (a == L"--fg-regionbench" && i + 2 < argc) {
            // 덤프 폴더, 결과 폴더, [x,y,w,h]
            regionBenchDump = argv[++i];
            regionBenchOut = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                float r[4];
                if (swscanf_s(argv[++i], L"%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4) std::copy(r, r + 4, regionBench);
            }
        }
        else if (a == L"--import-profile" && i + 1 < argc) importProfile = argv[++i];
        else if (a == L"--export-profile" && i + 2 < argc) {
            exportProfileName = argv[++i];
            exportProfileFile = argv[++i];
        }
        else if (a == L"--rife-bench" && i + 1 < argc) rifeBenchDir = argv[++i];
        else if (a == L"--fg-gaptest" && i + 2 < argc) {
            fgGapTestDump = argv[++i];
            fgGapTestOut = argv[++i];
            // 세 번째 값이 .json이면 비교할 게임 프로필이다.
            if (i + 1 < argc && std::wstring(argv[i + 1]).ends_with(L".json")) fgGapTestProfile = argv[++i];
        }
        else if (a == L"--fg") g_settings.frameGen = true;
        else if (a == L"--no-fg") g_settings.frameGen = false;  // 프레임 생성을 끈 기준 측정용
        else if (a == L"--fg-mult" && i + 1 < argc) g_settings.fgMultiplier = _wtoi(argv[++i]);
        else if (a == L"--fg-quality" && i + 1 < argc) g_settings.fgQuality = std::clamp(_wtoi(argv[++i]), 0, FG_QUALITY_COUNT - 1);
        else if (a == L"--fg-extrapolate") SetFgMode(g_settings, FG_MODE_EXTRAPOLATE);
        else if (a == L"--fg-rife") SetFgMode(g_settings, FG_MODE_RIFE);
        else if (a == L"--fg-block") SetFgMode(g_settings, FG_MODE_BLOCK);
        else if (a == L"--fg-visualize") g_settings.fgVisualize = true;
        else if (a == L"--no-gpu-priority") g_gpuPriorityAllowed = false;
        else if (a == L"--rife-gpu-priority" && i + 1 < argc) g_rifeGpuClass = std::clamp(_wtoi(argv[++i]), 0, 5);
        else if (a == L"--fg-log" && i + 1 < argc) _wfopen_s(&g_fgLog, argv[++i], L"w");
        else if (a == L"--slowmo-auto" && i + 1 < argc) slowMoAuto = _wtof(argv[++i]);
        else if (a == L"--region-edit") regionEditAuto = true;
        else if (a == L"--overlay-menu") overlayMenuAuto = true;
        else if (a == L"--region-wizard") g_autoOverlay = true;  // 테스트용: --no-gui에서도 확대를 시작하면 메뉴와 영역 팝업을 띄운다
        else if (a == L"--region-recommend" && i + 1 < argc) regionRecommendAt = _wtof(argv[++i]);
        else if (a == L"--fg-region" && i + 1 < argc) {
            // "x,y,w,h" (게임 화면에 대한 비율) 또는 "none"(화면 전체)
            const std::wstring v = argv[++i];
            float r[4] = { 0, 0, 0, 0 };
            if (v == L"none" || swscanf_s(v.c_str(), L"%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4) {
                r[0] = std::clamp(r[0], 0.0f, 1.0f);
                r[1] = std::clamp(r[1], 0.0f, 1.0f);
                r[2] = std::clamp(r[2], 0.0f, 1.0f - r[0]);
                r[3] = std::clamp(r[3], 0.0f, 1.0f - r[1]);
                std::copy(r, r + 4, g_fgRegionOverride);
            }
        }
        else if (a == L"--slowmo-target" && i + 1 < argc) { const int fps = _wtoi(argv[++i]); g_settings.slowMoTargetFps = fps <= 0 ? 0 : std::clamp(fps, 30, 1000); }
        else if (a == L"--slowmo-seconds" && i + 1 < argc) g_settings.slowMoSeconds = std::clamp(_wtoi(argv[++i]), 1, 3);
        else if (a == L"--slowmo-dump" && i + 1 < argc) g_slowMo.dumpDir = argv[++i];
    }

    // 시스템 커서를 숨기는 데 Magnification API를 사용한다.
    if (!MagInitialize()) printf("경고: Magnification API 초기화 실패. 커서가 두 개로 보일 수 있습니다.\n");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    int exitCode = 0;
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            MessageBoxW(nullptr, L"이 시스템은 화면 캡처(Windows.Graphics.Capture)를 지원하지 않습니다.\n"
                                 L"Windows 10 1903 이상이 필요합니다.", L"OpenScaler", MB_ICONERROR);
            return 1;
        }

        PrintHelp();
        printf("설정 파일: %s\n현재 설정: %s, 샤픈 %.1f, 정수배 %s\n\n", winrt::to_string(g_settingsPath).c_str(),
               kModeNames[g_settings.mode], g_settings.sharpness, g_settings.integerScale ? "ON" : "OFF");
        InitD3D();
        if (!fgSelfTestDir.empty()) {
            exitCode = RunFrameGenSelfTest(g_device.get(), g_ctx.get(), fgSelfTestDir);
            MagUninitialize();
            return exitCode;
        }
        if (!importProfile.empty()) {
            std::wstring name, message;
            const bool ok = AppImportProfile(importProfile, name, message);
            printf("[가져오기] %s: %s\n", winrt::to_string(name).c_str(), winrt::to_string(message).c_str());
            MagUninitialize();
            return ok ? 0 : 1;
        }
        if (!exportProfileName.empty()) {
            const bool ok = AppExportProfile(exportProfileName, exportProfileFile);
            printf("[내보내기] %s\n", ok ? "완료" : "실패");
            MagUninitialize();
            return ok ? 0 : 1;
        }
        if (!regionBenchDump.empty()) {
            exitCode = RunFrameGenRegionBench(g_device.get(), g_ctx.get(), regionBenchDump, regionBenchOut, regionBench);
            MagUninitialize();
            return exitCode;
        }
        if (!fgRegionTestDir.empty()) {
            exitCode = RunFrameGenRegionTest(g_device.get(), g_ctx.get(), fgRegionTestDir);
            MagUninitialize();
            return exitCode;
        }
        if (!rifeBenchDir.empty()) {
            exitCode = RunRifeBench(rifeBenchDir);
            MagUninitialize();
            return exitCode;
        }
        if (!fgGapTestDump.empty()) {
            exitCode = RunFrameGenGapTest(g_device.get(), g_ctx.get(), fgGapTestDump, fgGapTestOut, fgGapTestProfile);
            MagUninitialize();
            return exitCode;
        }
        RegisterHotkeys();
        if (useGui) g_autoOverlay = true;  // 모니터가 하나면 설정 창이 가려지므로 확대하면 메뉴와 영역 팝업을 띄운다
        g_frameGenLoading = std::async(std::launch::async, [] {
            auto frameGen = std::make_unique<FrameGen>(g_device.get(), g_ctx.get());
            frameGen->SetRealtime(true);
            return frameGen;
        });
        if (useGui && !GuiCreate(g_device.get(), g_ctx.get())) {
            printf("설정 창을 만들지 못했습니다. 단축키로만 사용할 수 있습니다.\n");
            useGui = false;
        }

        if (!targetTitle.empty()) {
            HWND hwnd = FindWindowByTitle(targetTitle);
            if (!hwnd) {
                printf("지정한 제목이 포함된 창을 찾지 못했습니다.\n");
                return 1;
            }
            StartScaling(hwnd);
            if (slowMoAuto >= 0) SlowMoToggle(slowMoAuto);
        }

        auto startTime = Clock::now();
        bool running = true;
        while (running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                else if (msg.message == WM_HOTKEY) running = HandleHotkey((int)msg.wParam) && running;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running) break;
            if (useGui && !GuiIsOpen()) break;  // 설정 창을 닫으면 종료

            if (duration > 0 && std::chrono::duration<double>(Clock::now() - startTime).count() > duration) break;

            UpdateGpuPriority();
            if (g_scaling) {
                if (!IsWindow(g_scaling->target)) {
                    StopScaling();
                    continue;
                }
                // 이전 프레임이 화면에 표시될 때까지 기다린 뒤, 가장 최신 캡처와 커서 위치로 그린다.
                WaitForSingleObjectEx(g_scaling->frameWaitable, 100, TRUE);
                RenderFrame();
                PrintStatus();
                if (regionEditAuto && g_scaling && g_scaling->layoutValid) {
                    RegionEditBegin();
                    regionEditAuto = false;
                }
                if (overlayMenuAuto && g_scaling && g_scaling->layoutValid) {
                    SetOverlayMenu(true);
                    overlayMenuAuto = false;
                }
                if (g_scaling && scalingSince < 0) scalingSince = NowSeconds();
                if (regionRecommendAt >= 0 && scalingSince >= 0 && NowSeconds() - scalingSince >= regionRecommendAt) {
                    float r[4] = {};
                    const int advice = (int)RegionRecommend(r);
                    printf("\n[영역 추천] 결과 %d (0: 추천함, 1: 모은 시간 부족, 2: 움직임 적음, 3: 거의 전체, -1: 프레임 생성 꺼짐), 모은 시간 %.1f초, 영역 %.3f,%.3f,%.3f,%.3f\n",
                           advice, g_frameGen ? g_frameGen->ChangeSeconds() : 0.0, r[0], r[1], r[2], r[3]);
                    if (g_frameGen) {
                        int gw = 0, gh = 0;
                        const std::vector<float> rates = g_frameGen->ChangeRates(gw, gh);
                        for (int y = 0; y < gh; y++) {
                            for (int x = 0; x < gw; x++) printf("%c", " .:-=+*#%@"[std::clamp((int)(rates[(size_t)y * gw + x] * 10), 0, 9)]);
                            printf("\n");
                        }
                    }
                    regionRecommendAt = -1;
                }
                if (useGui) GuiRender(true);  // 확대 화면 속도에 영향이 없도록 가끔만 그린다
            } else if (useGui && GuiIsVisible()) {
                GuiRender(false);  // 수직동기에 맞춰 그리므로 바쁜 루프가 되지 않는다
            } else {
                // 그릴 것이 없으면 메시지가 올 때까지 대기
                MsgWaitForMultipleObjects(0, nullptr, FALSE, duration > 0 ? 100 : INFINITE, QS_ALLINPUT);
            }
        }

        StopScaling();
        if (g_frameGenLoading.valid()) g_frameGenLoading.wait();
        g_frameGen.reset();  // 신경망 스레드를 D3D 정리 전에 끝낸다
        GuiOverlayShutdown();  // 설정 창 없이 실행한 경우 (설정 창이 있으면 GuiDestroy에서도 부른다)
        GuiDestroy();
    } catch (const winrt::hresult_error& e) {
        std::string text = "오류가 발생했습니다.\n\n" + winrt::to_string(e.message());
        printf("\n오류: 0x%08X %s\n", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
        MessageBoxA(nullptr, text.c_str(), "OpenScaler", MB_ICONERROR);
        exitCode = 1;
    }

    ShowSystemCursor(true);
    RestoreMouseSpeed();
    MagUninitialize();
    if (g_fgLog) fclose(g_fgLog);
    return exitCode;
}
