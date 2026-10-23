// OpenScaler - 창 모드 게임을 캡처해 전체화면으로 업스케일하는 프로토타입
//
// 흐름: 대상 창 캡처(Windows.Graphics.Capture)
//       -> 클라이언트 영역만 잘라 복사
//       -> Pass 1: 업스케일 셰이더 -> Pass 2: 샤프닝 셰이더 -> 커서 그리기
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
#include <dxgi1_2.h>
#include <d3dcompiler.h>

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
#include <string>
#include <unordered_map>
#include <vector>

#include "shaders.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::com_ptr;
using winrt::check_hresult;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// 설정
// ---------------------------------------------------------------------------

enum Mode { MODE_NEAREST, MODE_BILINEAR, MODE_BICUBIC, MODE_LANCZOS, MODE_COUNT };
static const char* kModeNames[MODE_COUNT] = { "Nearest", "Bilinear", "Bicubic", "Lanczos2" };

enum HotkeyId { HK_TOGGLE = 1, HK_MODE, HK_INTEGER, HK_SHARP_UP, HK_SHARP_DOWN, HK_QUIT };

struct Settings {
    int mode = MODE_LANCZOS;
    float sharpness = 0.5f;
    bool integerScale = false;
};

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

// D3D 공용 리소스
static com_ptr<ID3D11Device> g_device;
static com_ptr<ID3D11DeviceContext> g_ctx;
static wgd3d::IDirect3DDevice g_winrtDevice{ nullptr };
static com_ptr<ID3D11VertexShader> g_vs;
static com_ptr<ID3D11PixelShader> g_psScale;
static com_ptr<ID3D11PixelShader> g_psSharpen;
static com_ptr<ID3D11PixelShader> g_psCursor;
static com_ptr<ID3D11SamplerState> g_sampler;
static com_ptr<ID3D11Buffer> g_cbuffer;
static com_ptr<ID3D11BlendState> g_blendAlpha;   // 일반 커서 픽셀: 알파 블렌딩
static com_ptr<ID3D11BlendState> g_blendInvert;  // 흑백 커서의 반전 픽셀: 1 - 배경색

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

    // 통계
    int capturedFrames = 0;
    int presentedFrames = 0;
    Clock::time_point statTime;
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

    g_blendAlpha = CreateBlend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    // 셰이더 출력이 흰색(1)이면 1 - 배경, 검은색(0)이면 배경 그대로
    g_blendInvert = CreateBlend(D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_INV_SRC_COLOR);
}

// ---------------------------------------------------------------------------
// 오버레이 창
// ---------------------------------------------------------------------------

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
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

static void StopScaling()
{
    if (!g_scaling) return;
    if (g_scaling->cursorMapped) {
        POINT p;
        GetCursorPos(&p);
        ReleaseCursor(g_scaling, SrcToDst(g_scaling, ClampToRect(p, g_scaling->srcScreen)));
    }
    ShowSystemCursor(true);
    if (g_scaling->session) g_scaling->session.Close();
    if (g_scaling->framePool) g_scaling->framePool.Close();
    g_ctx->ClearState();
    g_ctx->Flush();
    if (g_scaling->overlay) DestroyWindow(g_scaling->overlay);
    delete g_scaling;
    g_scaling = nullptr;
    printf("\n[중지] 스케일링을 종료했습니다.\n");
}

static void StartScaling(HWND target)
{
    StopScaling();
    if (!target || !IsWindow(target)) return;

    wchar_t title[256] = {};
    GetWindowTextW(target, title, ARRAYSIZE(title));

    auto s = new ScalingSession();
    s->target = target;

    // 대상 창이 있는 모니터 전체를 덮는다.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
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
    check_hresult(factory->CreateSwapChainForHwnd(g_device.get(), s->overlay, &scd, nullptr, nullptr, s->swapChain.put()));
    factory->MakeWindowAssociation(s->overlay, DXGI_MWA_NO_ALT_ENTER);

    com_ptr<ID3D11Texture2D> backBuffer;
    check_hresult(s->swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));
    check_hresult(g_device->CreateRenderTargetView(backBuffer.get(), nullptr, s->backBufferRtv.put()));

    s->session.StartCapture();
    s->statTime = Clock::now();
    g_scaling = s;

    char titleUtf8[768] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, titleUtf8, sizeof(titleUtf8), nullptr, nullptr);
    printf("\n[시작] \"%s\" -> %dx%d 전체화면\n", titleUtf8, s->outW, s->outH);
}

// ---------------------------------------------------------------------------
// 렌더링
// ---------------------------------------------------------------------------

// 캡처 텍스처 안에서 대상 창의 클라이언트 영역(제목 표시줄/테두리 제외) 위치를 구한다.
static bool GetClientCrop(HWND hwnd, UINT texW, UINT texH, D3D11_BOX& box)
{
    RECT frame;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) return false;
    RECT client;
    GetClientRect(hwnd, &client);
    POINT tl = { 0, 0 };
    ClientToScreen(hwnd, &tl);

    LONG left = std::clamp<LONG>(tl.x - frame.left, 0, texW);
    LONG top = std::clamp<LONG>(tl.y - frame.top, 0, texH);
    LONG right = std::clamp<LONG>(left + client.right, 0, texW);
    LONG bottom = std::clamp<LONG>(top + client.bottom, 0, texH);
    if (right <= left || bottom <= top) return false;

    box = { (UINT)left, (UINT)top, 0, (UINT)right, (UINT)bottom, 1 };
    return true;
}

// 원본 창 영역과 화면 비율을 유지한 확대 출력 영역을 화면 좌표로 계산한다.
static void UpdateLayout(ScalingSession* s)
{
    if (!s->srcTex) return;

    POINT tl = { 0, 0 };
    ClientToScreen(s->target, &tl);
    s->srcScreen = { tl.x, tl.y, tl.x + (LONG)s->srcW, tl.y + (LONG)s->srcH };

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

static void SetParams(UINT w, UINT h)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(g_ctx->Map(g_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    auto p = static_cast<ShaderParams*>(mapped.pData);
    p->srcSize[0] = (float)w;
    p->srcSize[1] = (float)h;
    p->invSrcSize[0] = 1.0f / w;
    p->invSrcSize[1] = 1.0f / h;
    p->mode = g_settings.mode;
    p->sharpness = g_settings.sharpness;
    g_ctx->Unmap(g_cbuffer.get(), 0);
}

static void DrawPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps,
                     ID3D11ShaderResourceView* srv, UINT srcW, UINT srcH)
{
    SetParams(srcW, srcH);
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

static void RenderFrame()
{
    auto s = g_scaling;

    // 1. 가장 최신 캡처 프레임만 사용한다.
    wgc::Direct3D11CaptureFrame frame{ nullptr };
    while (auto next = s->framePool.TryGetNextFrame()) {
        frame = next;
        s->capturedFrames++;
    }

    if (frame) {
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
        if (GetClientCrop(s->target, contentSize.Width, contentSize.Height, box)) {
            EnsureTexture(s->srcTex, s->srcSrv, nullptr, s->srcW, s->srcH, box.right - box.left, box.bottom - box.top);
            g_ctx->CopySubresourceRegion(s->srcTex.get(), 0, 0, 0, 0, frameTex.get(), 0, &box);
        }
    }

    // 2. 출력 영역 계산과 커서 위치 처리
    UpdateLayout(s);
    UpdateCursor(s);

    float clear[4] = { 0, 0, 0, 1 };
    g_ctx->ClearRenderTargetView(s->backBufferRtv.get(), clear);

    if (s->layoutValid) {
        UINT dstW = s->dstScreen.right - s->dstScreen.left;
        UINT dstH = s->dstScreen.bottom - s->dstScreen.top;
        float offX = (float)(s->dstScreen.left - s->monitor.left);
        float offY = (float)(s->dstScreen.top - s->monitor.top);

        EnsureTexture(s->midTex, s->midSrv, &s->midRtv, s->midW, s->midH, dstW, dstH);

        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_vs.get(), nullptr, 0);
        ID3D11SamplerState* sampler = g_sampler.get();
        g_ctx->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* cb = g_cbuffer.get();
        g_ctx->PSSetConstantBuffers(0, 1, &cb);

        // Pass 1: 업스케일
        D3D11_VIEWPORT vp1 = { 0, 0, (float)dstW, (float)dstH, 0, 1 };
        DrawPass(s->midRtv.get(), vp1, g_psScale.get(), s->srcSrv.get(), s->srcW, s->srcH);

        // Pass 2: 샤프닝하면서 백버퍼 중앙에 출력
        D3D11_VIEWPORT vp2 = { offX, offY, (float)dstW, (float)dstH, 0, 1 };
        DrawPass(s->backBufferRtv.get(), vp2, g_psSharpen.get(), s->midSrv.get(), dstW, dstH);

        // Pass 3: 커서
        DrawCursor(s);
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

    printf("\r  %ux%u -> %ux%u | 모드: %-8s | 샤픈: %.1f | 정수배: %-3s | 커서: %-4s | 캡처 %3.0f fps | 출력 %3.0f fps   ",
           g_scaling->srcW, g_scaling->srcH, g_scaling->midW, g_scaling->midH,
           kModeNames[g_settings.mode], g_settings.sharpness, g_settings.integerScale ? "ON" : "OFF",
           g_scaling->cursorMapped ? "변환" : "일반",
           g_scaling->capturedFrames / sec, g_scaling->presentedFrames / sec);
    fflush(stdout);
    g_scaling->capturedFrames = 0;
    g_scaling->presentedFrames = 0;
    g_scaling->statTime = now;
}

static bool HandleHotkey(int id)
{
    switch (id) {
    case HK_TOGGLE:
        if (g_scaling) StopScaling();
        else StartScaling(GetForegroundWindow());
        break;
    case HK_MODE:
        g_settings.mode = (g_settings.mode + 1) % MODE_COUNT;
        break;
    case HK_INTEGER:
        g_settings.integerScale = !g_settings.integerScale;
        break;
    case HK_SHARP_UP:
        g_settings.sharpness = std::min(1.0f, g_settings.sharpness + 0.1f);
        break;
    case HK_SHARP_DOWN:
        g_settings.sharpness = std::max(0.0f, g_settings.sharpness - 0.1f);
        break;
    case HK_QUIT:
        return false;
    }
    return true;
}

static void RegisterHotkeys()
{
    const UINT mods = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    struct { int id; UINT vk; const char* name; } keys[] = {
        { HK_TOGGLE, 'S', "Alt+Shift+S" },        { HK_MODE, 'M', "Alt+Shift+M" },
        { HK_INTEGER, 'I', "Alt+Shift+I" },       { HK_SHARP_UP, VK_UP, "Alt+Shift+Up" },
        { HK_SHARP_DOWN, VK_DOWN, "Alt+Shift+Down" }, { HK_QUIT, 'Q', "Alt+Shift+Q" },
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
    return FALSE;
}

static void PrintHelp()
{
    printf("OpenScaler 프로토타입\n"
           "------------------------------------------------------------\n"
           "  Alt+Shift+S     현재 활성 창 스케일링 시작/중지\n"
           "  Alt+Shift+M     업스케일 모드 변경 (Nearest/Bilinear/Bicubic/Lanczos2)\n"
           "  Alt+Shift+I     정수배 스케일링 켜기/끄기\n"
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

    // 명령줄 옵션: --target "창 제목 일부" --duration 초 --mode 0~3 --sharp 0~1 --integer
    std::wstring targetTitle;
    double duration = 0;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--target" && i + 1 < argc) targetTitle = argv[++i];
        else if (a == L"--duration" && i + 1 < argc) duration = _wtof(argv[++i]);
        else if (a == L"--mode" && i + 1 < argc) g_settings.mode = std::clamp(_wtoi(argv[++i]), 0, MODE_COUNT - 1);
        else if (a == L"--sharp" && i + 1 < argc) g_settings.sharpness = std::clamp((float)_wtof(argv[++i]), 0.0f, 1.0f);
        else if (a == L"--integer") g_settings.integerScale = true;
    }

    // 시스템 커서를 숨기는 데 Magnification API를 사용한다.
    if (!MagInitialize()) printf("경고: Magnification API 초기화 실패. 커서가 두 개로 보일 수 있습니다.\n");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    int exitCode = 0;
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            printf("이 시스템은 Windows.Graphics.Capture를 지원하지 않습니다 (Windows 10 1903 이상 필요).\n");
            return 1;
        }

        PrintHelp();
        InitD3D();
        RegisterHotkeys();

        if (!targetTitle.empty()) {
            HWND hwnd = FindWindowByTitle(targetTitle);
            if (!hwnd) {
                printf("지정한 제목이 포함된 창을 찾지 못했습니다.\n");
                return 1;
            }
            StartScaling(hwnd);
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

            if (duration > 0 && std::chrono::duration<double>(Clock::now() - startTime).count() > duration) break;

            if (g_scaling) {
                if (!IsWindow(g_scaling->target)) {
                    StopScaling();
                    continue;
                }
                RenderFrame();  // Present(1, 0)이 수직동기까지 대기하므로 바쁜 루프가 되지 않는다.
                PrintStatus();
            } else {
                // 스케일링 중이 아니면 메시지가 올 때까지 대기
                MsgWaitForMultipleObjects(0, nullptr, FALSE, duration > 0 ? 100 : INFINITE, QS_ALLINPUT);
            }
        }

        StopScaling();
    } catch (const winrt::hresult_error& e) {
        printf("\n오류: 0x%08X %s\n", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
        exitCode = 1;
    }

    ShowSystemCursor(true);
    MagUninitialize();
    return exitCode;
}
