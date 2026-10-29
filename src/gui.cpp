// OpenScaler 설정 창
//
// Dear ImGui로 대상 창 선택, 시작/중지, 업스케일 설정, 상태를 한 화면에 보여 준다.
// 디자인: 흰 바탕 + 연분홍 픽셀 스타일 (두꺼운 테두리, 각진 모서리, 픽셀 글꼴 Galmuri11 Bold).
// 픽셀 글꼴은 12px 기준으로 그려졌으므로, 정수 배율 P(보통 2)로만 키워 픽셀이 뭉개지지 않게 한다.
// 캡처/렌더링과 같은 D3D11 디바이스를 쓰고, 메인 루프(main.cpp)에서 매 반복마다 GuiRender를 부른다.

#include "gui.h"
#include "app.h"
#include "resource.h"

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
HWND g_hwnd = nullptr;
ComPtr<IDXGISwapChain1> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_rtv;
bool g_open = false;
bool g_minimized = false;
float g_dpiScale = 1.0f;   // Windows 배율 (125% = 1.25)
float g_pendingDpi = 0;    // 배율이 다른 모니터로 옮겨지면 다음 프레임에 글꼴과 크기를 다시 만든다
int P = 2;                 // 픽셀 하나의 크기 (글꼴 12px x P)
Clock::time_point g_lastRender;

ImFont* g_fontBody = nullptr;   // 옵션 이름, 버튼 (Galmuri11 Bold)
ImFont* g_fontTitle = nullptr;  // 제목 (Galmuri11 Bold 2배)
ImFont* g_fontThin = nullptr;   // 설명 (Galmuri11 Condensed)
ComPtr<ID3D11ShaderResourceView> g_logo;
int g_logoSize = 0;

// 색상: 흰 바탕 + 연분홍
ImVec4 Rgb(int hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}
const ImVec4 kWhite = Rgb(0xFFFFFF);
const ImVec4 kPink = Rgb(0xF49AB8);       // 테두리, 버튼
const ImVec4 kPinkHover = Rgb(0xFFE6EF);  // 마우스를 올렸을 때 채우는 색
const ImVec4 kPinkMid = Rgb(0xF8BDD0);
const ImVec4 kPinkText = Rgb(0xE0648D);   // 본문 글자
const ImVec4 kPinkSoft = Rgb(0xE584A6);   // 설명 글자 (얇은 글꼴이라 너무 연하면 읽기 어렵다)
const ImVec4 kPinkDeep = Rgb(0xD81B60);   // 강조 (로고의 사선과 같은 색)

// 구역별 색 묶음. 대상 창(가장 중요한 곳)은 분홍, 나머지 설정은 눈에 덜 띄게 회색 단계로 칠한다.
struct Palette {
    ImVec4 border;  // 상자와 입력 칸 테두리
    ImVec4 title;   // "* 구역 제목" (가장 진하게)
    ImVec4 text;    // 옵션 이름
    ImVec4 soft;    // 설명 글 (가장 연하게)
    ImVec4 hover;   // 마우스를 올렸을 때 채우는 색
    ImVec4 active;  // 누르고 있을 때 / 선택된 항목
    ImVec4 fill;    // 체크 표시, 슬라이더 손잡이
    ImVec4 fillActive;
};
const Palette kPinkPalette = { kPink, kPinkDeep, kPinkText, kPinkSoft, kPinkHover, kPinkMid, kPink, kPinkDeep };
const Palette kGrayPalette = { Rgb(0xD0D0D0), Rgb(0x666666), Rgb(0x7D7D7D), Rgb(0xA3A3A3),
                               Rgb(0xF3F3F3), Rgb(0xE4E4E4), Rgb(0x9C9C9C), Rgb(0x6E6E6E) };
const Palette* g_pal = &kPinkPalette;

// 지금부터 그리는 위젯에 색 묶음을 적용한다 (PopPalette로 되돌린다)
void PushPalette(const Palette& p)
{
    g_pal = &p;
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.soft);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.active);
    ImGui::PushStyleColor(ImGuiCol_Header, p.active);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, p.hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, p.active);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, p.fill);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, p.fillActive);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, p.fill);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, p.hover);
}

void PopPalette()
{
    ImGui::PopStyleColor(14);
    g_pal = &kPinkPalette;
}

const char* kModeDescriptions[MODE_COUNT] = {
    "픽셀을 그대로 키웁니다. 픽셀 아트용.",
    "부드럽지만 흐립니다. 그냥 늘린 화면과 같아요.",
    "3차 곡선 보간. Bilinear보다 선명합니다.",
    "선명한 sinc 보간 + 테두리 번짐 억제.",
    "AMD FSR 1.0. 윤곽선이 가장 깔끔해요. (추천)",
};

// 설명 글 (얇은 글꼴로 표시)
const char* kSubtitle = "v" OPENSCALER_VERSION "  ·  창 모드 게임 업스케일러";
const char* kSharpnessDesc = "확대로 흐려진 윤곽선을 또렷하게 해요. 너무 높으면 테두리가 하얗게 번져요.";
const char* kIntegerDesc = "2배, 3배처럼 정수 배율로만 키워 픽셀이 고르게 커져요. 픽셀 아트용, 가장자리에 여백이 생길 수 있어요.";
const char* kFpsFormat = "출력 %.0f fps · 캡처 %.0f fps";
const char* kFooter = "설정은 OpenScaler.ini에 자동 저장돼요";
const char* kSecondaryTexts[] = { kSubtitle, kSharpnessDesc, kIntegerDesc, kFpsFormat, kFooter, "(?)" };

const char* kHotkeys[][2] = {
    { "Alt+Shift+S", "확대 시작/중지" },
    { "Alt+Shift+M", "알고리즘 변경" },
    { "Alt+Shift+↑↓", "샤프닝 조절" },
    { "Alt+Shift+I", "정수배 스케일링" },
    { "Alt+Shift+C", "비교 모드" },
    { "Alt+Shift+O", "상태 표시 항상 켜기" },
    { "Alt+Shift+P", "커서 속도 보정" },
    { "Alt+Shift+Q", "종료" },
};

// ---------------------------------------------------------------------------
// 대상 창 목록
// ---------------------------------------------------------------------------

struct WindowEntry {
    HWND hwnd;
    std::string label;
};
std::vector<WindowEntry> g_windows;
HWND g_selected = nullptr;

std::string ToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ProcessName(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        name.assign(path, len);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(proc);
    return name;
}

std::string WindowLabel(HWND hwnd)
{
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    std::wstring t = title;
    if (t.size() > 40) t = t.substr(0, 39) + L"…";
    std::wstring proc = ProcessName(hwnd);
    return ToUtf8(proc.empty() ? t : t + L"  (" + proc + L")");
}

// 확대할 만한 창만 고른다: 보이고, 제목이 있고, 도구 창/숨은 앱 창이 아니고, 너무 작지 않은 창
bool IsCandidateWindow(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER)) return false;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    return rc.right >= 160 && rc.bottom >= 120;
}

void RefreshWindows()
{
    g_windows.clear();
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (IsCandidateWindow(hwnd)) g_windows.push_back({ hwnd, WindowLabel(hwnd) });
        return TRUE;
    }, 0);
    if (g_selected && !IsWindow(g_selected)) g_selected = nullptr;
}

// ---------------------------------------------------------------------------
// 리소스: 로고(아이콘)와 픽셀 글꼴
// ---------------------------------------------------------------------------

// 실행 파일의 아이콘을 원하는 크기로 꺼내 텍스처로 만든다.
void LoadLogo(int size)
{
    g_logo.Reset();
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0);
    if (!icon) return;
    ICONINFO ii;
    if (GetIconInfo(icon, &ii)) {
        std::vector<uint32_t> px((size_t)size * size);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, size, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = size;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { px.data(), (UINT)size * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(g_device->CreateTexture2D(&td, &init, &tex)))
            g_device->CreateShaderResourceView(tex.Get(), nullptr, &g_logo);
        g_logoSize = size;
    }
    DestroyIcon(icon);
}

bool LoadFontResource(int id, void** data, int* size)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    *data = mem ? LockResource(mem) : nullptr;
    *size = (int)SizeofResource(nullptr, res);
    return *data != nullptr;
}

void SetupFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // 한글 + 화면에 쓰는 기호
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
        builder.AddText("→↑↓…♥·×▼");
        builder.BuildRanges(&ranges);
    }

    // 설명 글꼴은 정해진 문장만 쓰므로, 그 문장에 나오는 글자만 담아 글꼴 텍스처를 작게 유지한다.
    static ImVector<ImWchar> thinRanges;
    if (thinRanges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddText("→↑↓…♥·×▼");
        for (const char* text : kModeDescriptions) builder.AddText(text);
        for (const char* text : kSecondaryTexts) builder.AddText(text);
        builder.BuildRanges(&thinRanges);
    }

    // 픽셀 글꼴: 12px의 정수 배로만 그려야 각진 모양이 유지된다.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // 리소스 메모리라 해제하면 안 된다
    cfg.OversampleH = cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    void* data = nullptr;
    int size = 0;
    if (LoadFontResource(IDR_PIXEL_FONT, &data, &size)) {
        g_fontBody = io.Fonts->AddFontFromMemoryTTF(data, size, 12.0f * P, &cfg, ranges.Data);
        g_fontTitle = io.Fonts->AddFontFromMemoryTTF(data, size, 24.0f * P, &cfg, io.Fonts->GetGlyphRangesDefault());
    } else {
        ImFontConfig def;
        def.SizePixels = 13.0f * P;
        g_fontBody = g_fontTitle = io.Fonts->AddFontDefault(&def);
    }

    // 설명 글은 얇은 글꼴(Galmuri11 Condensed, 1px 획)로 옵션 이름(Bold)과 구분한다.
    if (LoadFontResource(IDR_PIXEL_FONT_THIN, &data, &size))
        g_fontThin = io.Fonts->AddFontFromMemoryTTF(data, size, 12.0f * P, &cfg, thinRanges.Data);
    else
        g_fontThin = g_fontBody;
}

void SetupStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsLight(&st);

    const float u = (float)P;  // 픽셀 단위
    st.WindowPadding = ImVec2(10 * u, 6 * u);
    st.FramePadding = ImVec2(5 * u, 3 * u);
    st.ItemSpacing = ImVec2(5 * u, 3 * u);
    st.ItemInnerSpacing = ImVec2(4 * u, 3 * u);
    st.IndentSpacing = 10 * u;
    st.ScrollbarSize = 8 * u;
    st.GrabMinSize = 6 * u;
    st.SeparatorTextBorderSize = u;
    st.SeparatorTextPadding = ImVec2(0, 2 * u);

    // 각지고 두꺼운 테두리
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0;
    st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 2 * u;
    st.FrameBorderSize = u;
    st.PopupBorderSize = u;

    // 선과 도형의 가장자리를 부드럽게 하지 않는다 (픽셀 느낌)
    st.AntiAliasedLines = false;
    st.AntiAliasedLinesUseTex = false;
    st.AntiAliasedFill = false;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kPinkText;
    c[ImGuiCol_TextDisabled] = kPinkSoft;
    c[ImGuiCol_WindowBg] = kWhite;
    c[ImGuiCol_ChildBg] = kWhite;
    c[ImGuiCol_PopupBg] = kWhite;
    c[ImGuiCol_Border] = kPink;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kWhite;
    c[ImGuiCol_FrameBgHovered] = kPinkHover;
    c[ImGuiCol_FrameBgActive] = kPinkHover;
    c[ImGuiCol_Button] = kWhite;
    c[ImGuiCol_ButtonHovered] = kPinkHover;
    c[ImGuiCol_ButtonActive] = kPinkMid;
    c[ImGuiCol_Header] = kPinkMid;
    c[ImGuiCol_HeaderHovered] = kPinkHover;
    c[ImGuiCol_HeaderActive] = kPinkMid;
    c[ImGuiCol_CheckMark] = kPink;
    c[ImGuiCol_SliderGrab] = kPink;
    c[ImGuiCol_SliderGrabActive] = kPinkDeep;
    c[ImGuiCol_Separator] = kPink;
    c[ImGuiCol_SeparatorHovered] = kPink;
    c[ImGuiCol_SeparatorActive] = kPink;
    c[ImGuiCol_ScrollbarBg] = kWhite;
    c[ImGuiCol_ScrollbarGrab] = kPinkMid;
    c[ImGuiCol_ScrollbarGrabHovered] = kPink;
    c[ImGuiCol_ScrollbarGrabActive] = kPinkDeep;
    c[ImGuiCol_TableRowBg] = kWhite;
    c[ImGuiCol_TableRowBgAlt] = kPinkHover;
    c[ImGuiCol_TableBorderLight] = kPinkMid;
    c[ImGuiCol_TableBorderStrong] = kPink;
    c[ImGuiCol_NavHighlight] = kPinkDeep;
}

// Windows 배율에 맞는 픽셀 크기: 100%~150%는 2배, 175% 이상은 3배
int PixelScaleFor(float dpiScale) { return std::max(1, (int)std::lround(dpiScale * 1.6f)); }

// 배율에 맞춰 픽셀 크기, 스타일, 글꼴, 로고를 다시 만든다.
void ApplyScale(float dpiScale)
{
    g_dpiScale = dpiScale;
    P = PixelScaleFor(dpiScale);
    SetupStyle();
    SetupFonts();
    LoadLogo(26 * P);
}

// ---------------------------------------------------------------------------
// 창 / 스왑체인
// ---------------------------------------------------------------------------

void CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_rtv);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;

    switch (msg) {
    case WM_SIZE:
        g_minimized = (wp == SIZE_MINIMIZED);
        if (!g_minimized && g_swapChain) {
            // 백버퍼를 붙잡고 있는 참조를 모두 놓아야 크기를 바꿀 수 있다.
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = { (LONG)(280 * P), (LONG)(300 * P) };
        return 0;
    }
    case WM_DPICHANGED: {
        // 픽셀 크기 P가 그대로면(예: 125% -> 100%) 창 크기도 그대로 두고 위치만 옮긴다.
        g_pendingDpi = HIWORD(wp) / 96.0f;
        int newP = PixelScaleFor(g_pendingDpi);
        auto rc = reinterpret_cast<RECT*>(lp);
        RECT cur;
        GetWindowRect(hwnd, &cur);
        int w = (cur.right - cur.left) * newP / P;
        int h = (cur.bottom - cur.top) * newP / P;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt 키로 메뉴가 열리지 않게
        break;
    case WM_CLOSE:
        g_open = false;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 픽셀 스타일 위젯
// ---------------------------------------------------------------------------

ImU32 Col(const ImVec4& v) { return ImGui::GetColorU32(v); }

// 7x6 픽셀 하트
void DrawHeart(ImDrawList* dl, ImVec2 pos, ImU32 color)
{
    static const char* rows[] = { ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..." };
    const float u = (float)P;
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 7; x++)
            if (rows[y][x] == 'X')
                dl->AddRectFilled(ImVec2(pos.x + x * u, pos.y + y * u), ImVec2(pos.x + (x + 1) * u, pos.y + (y + 1) * u), color);
}

// 테두리 상자 안에 내용을 넣는다. 제목은 언더테일 대사처럼 "* "로 시작한다.
void BeginBox(const char* id, const char* title, const Palette& palette)
{
    PushPalette(palette);
    ImGui::BeginChild(id, ImVec2(-FLT_MIN, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.title);
    ImGui::Text("* %s", title);
    ImGui::PopStyleColor();
}

void EndBox()
{
    ImGui::EndChild();
    PopPalette();
    ImGui::Dummy(ImVec2(0, 2.0f * P));
}

// 하트가 붙은 큰 버튼 (filled: 분홍으로 채운 주요 버튼)
bool HeartButton(const char* label, ImVec4 fill, ImVec4 hover, ImVec4 active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    ImGui::PushStyleColor(ImGuiCol_Border, kPinkDeep);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * P);
    bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, 24.0f * P));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);

    // 글자 왼쪽에 하트
    ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    float textW = ImGui::CalcTextSize(label, nullptr, true).x;
    float cx = (min.x + max.x) * 0.5f - textW * 0.5f - 12.0f * P;
    float cy = (min.y + max.y) * 0.5f - 3.0f * P;
    DrawHeart(ImGui::GetWindowDrawList(), ImVec2(std::floor(cx), std::floor(cy)), Col(kWhite));
    return pressed;
}

// 네모 칸 체크박스: 켜지면 안쪽을 채운다 (색은 현재 색 묶음)
bool PixelCheckbox(const char* label, bool* value)
{
    const float u = (float)P;
    const float box = 10 * u;
    const float textH = ImGui::GetTextLineHeight();
    ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = std::max(box, textH);

    bool pressed = ImGui::InvisibleButton(label, ImVec2(box + 5 * u + textSize.x, h));
    if (pressed) *value = !*value;
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b0(pos.x, pos.y + std::floor((h - box) * 0.5f));
    ImVec2 b1(b0.x + box, b0.y + box);
    const Palette& p = *g_pal;
    dl->AddRectFilled(b0, b1, Col(hovered ? p.hover : kWhite));
    dl->AddRectFilled(b0, ImVec2(b1.x, b0.y + u), Col(p.fill));
    dl->AddRectFilled(ImVec2(b0.x, b1.y - u), b1, Col(p.fill));
    dl->AddRectFilled(b0, ImVec2(b0.x + u, b1.y), Col(p.fill));
    dl->AddRectFilled(ImVec2(b1.x - u, b0.y), b1, Col(p.fill));
    if (*value) dl->AddRectFilled(ImVec2(b0.x + 3 * u, b0.y + 3 * u), ImVec2(b1.x - 3 * u, b1.y - 3 * u), Col(p.fillActive));
    dl->AddText(ImVec2(b1.x + 5 * u, pos.y + std::floor((h - textH) * 0.5f)), Col(p.text), label);
    return pressed;
}

// 설명 글: 얇은 글꼴 + 현재 색 묶음의 연한 색, 줄바꿈
void Description(const char* text)
{
    ImGui::PushFont(g_fontThin);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal->soft);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// 바로 앞 위젯 옆에 (?)를 붙이고, 마우스를 올리면 설명을 보여 준다.
void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::PushFont(g_fontThin);
    ImGui::TextDisabled("(?)");
    ImGui::PopFont();
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// 왼쪽에 이름, 오른쪽에 입력 위젯이 오는 한 줄
const float kLabelWidth = 52;
void RowLabel(const char* label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelWidth * P + ImGui::GetStyle().WindowPadding.x);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// ---------------------------------------------------------------------------
// 화면 구성
// ---------------------------------------------------------------------------

void DrawUi()
{
    Settings& s = AppSettings();
    ScalingStatus status = AppGetStatus();
    bool changed = false;
    const float u = (float)P;

    if (status.active) g_selected = status.target;
    if (g_selected && !IsWindow(g_selected)) g_selected = nullptr;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 머리말: 로고 + 이름 + 버전
    if (g_logo) {
        ImGui::Image((ImTextureID)g_logo.Get(), ImVec2((float)g_logoSize, (float)g_logoSize));
        ImGui::SameLine(0, 6 * u);
    }
    ImGui::BeginGroup();
    // 제목: "Open"은 연분홍, "Scaler"는 진한 분홍
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kPink, "Open");
    ImGui::SameLine(0, 0);
    ImGui::TextColored(kPinkDeep, "Scaler");
    ImGui::PopFont();
    Description(kSubtitle);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 2 * u));

    // 대상 창과 시작/중지
    BeginBox("##target", "대상 창", kPinkPalette);
    {
        const char* refreshLabel = "새로고침";
        float refreshWidth = ImGui::CalcTextSize(refreshLabel).x + ImGui::GetStyle().FramePadding.x * 2;
        std::string preview = g_selected ? WindowLabel(g_selected) : "확대할 창을 고르세요";
        ImGui::SetNextItemWidth(-refreshWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(status.active);
        if (ImGui::BeginCombo("##target", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) RefreshWindows();
            if (g_windows.empty()) ImGui::TextDisabled("확대할 수 있는 창이 없어요");
            for (auto& w : g_windows) {
                if (ImGui::Selectable(w.label.c_str(), w.hwnd == g_selected)) g_selected = w.hwnd;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(refreshLabel)) RefreshWindows();
        ImGui::EndDisabled();

        if (status.active) {
            if (HeartButton("확대 중지", kPinkDeep, Rgb(0xE5487F), Rgb(0xB0124C))) AppStopScaling();
        } else {
            ImGui::BeginDisabled(!g_selected);
            if (HeartButton("확대 시작", kPink, Rgb(0xF7AEC7), kPinkDeep)) {
                HWND target = g_selected;
                if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
                AppStartScaling(target);
                SetForegroundWindow(target);  // 게임 창이 활성 상태여야 마우스 좌표 변환이 동작한다
            }
            ImGui::EndDisabled();
        }

        // 상태: 확대 중이면 진한 하트, 대기 중이면 연한 하트
        ImVec2 p = ImGui::GetCursorScreenPos();
        float lineH = ImGui::GetTextLineHeight();
        DrawHeart(ImGui::GetWindowDrawList(), ImVec2(p.x, std::floor(p.y + (lineH - 6 * u) * 0.5f)),
                  Col(status.active ? kPinkDeep : kPinkMid));
        ImGui::Dummy(ImVec2(7 * u, lineH));
        ImGui::SameLine();
        if (status.active) {
            ImGui::Text("확대 중  %u×%u → %u×%u", status.srcW, status.srcH, status.dstW, status.dstH);
            char fps[64];
            snprintf(fps, sizeof(fps), kFpsFormat, status.outputFps, status.captureFps);
            Description(fps);
            HelpMarker("확대 화면이 이 창을 가리면 Alt+Shift+S로 중지할 수 있어요.");
        } else {
            ImGui::TextUnformatted("대기 중");
            HelpMarker("게임을 창 모드로, 모니터보다 낮은 해상도로 실행한 뒤 위 목록에서 고르세요.\n"
                       "게임 창에서 Alt+Shift+S를 눌러도 시작돼요.\n"
                       "확대 화면이 이 창을 가리면 Alt+Shift+S로 중지합니다.");
        }
    }
    EndBox();

    // 업스케일 (여기부터는 회색으로 눈에 덜 띄게)
    BeginBox("##upscale", "업스케일", kGrayPalette);
    RowLabel("알고리즘");
    if (ImGui::Combo("##mode", &s.mode, kModeNames, MODE_COUNT)) changed = true;
    Description(kModeDescriptions[s.mode]);

    RowLabel("샤프닝");
    ImGui::SliderFloat("##sharp", &s.sharpness, 0.0f, 1.0f, s.sharpness <= 0.0f ? "끔" : "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    Description(kSharpnessDesc);

    if (PixelCheckbox("정수배 스케일링", &s.integerScale)) changed = true;
    Description(kIntegerDesc);
    EndBox();

    // 화면 표시와 마우스
    BeginBox("##options", "화면 표시 · 마우스", kGrayPalette);
    if (PixelCheckbox("비교 모드", &s.compare)) changed = true;
    HelpMarker("화면을 반으로 나눠 왼쪽은 일반 확대(Bilinear), 오른쪽은 지금 알고리즘으로 보여 줘요.");
    if (PixelCheckbox("상태 표시 항상 켜기", &s.osdAlways)) changed = true;
    HelpMarker("확대 화면 왼쪽 위에 알고리즘, 해상도, FPS를 계속 표시해요.\n끄면 설정을 바꿀 때만 잠깐 표시해요.");
    if (PixelCheckbox("커서 속도 보정", &s.adjustCursorSpeed)) changed = true;
    HelpMarker("확대하면 커서가 배율만큼 빨라져요.\n켜면 확대 중에만 Windows 마우스 속도를 낮춰 원래 속도에 맞추고, "
               "끝나면 되돌려요.");
    EndBox();

    // 단축키 (회색)
    PushPalette(kGrayPalette);
    ImGui::PushStyleColor(ImGuiCol_Text, kGrayPalette.title);
    bool showHotkeys = ImGui::CollapsingHeader("단축키");
    ImGui::PopStyleColor();
    if (showHotkeys) {
        if (ImGui::BeginTable("##hotkeys", 2, ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 80 * u);
            for (auto& h : kHotkeys) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h[1]);
            }
            ImGui::EndTable();
        }
    }

    ImGui::Dummy(ImVec2(0, 2 * u));
    Description(kFooter);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(ToUtf8(AppSettingsPath()).c_str());
        ImGui::EndTooltip();
    }
    PopPalette();

    ImGui::End();

    if (changed) AppSettingsChanged();
}

}  // namespace

// ---------------------------------------------------------------------------
// gui.h
// ---------------------------------------------------------------------------

bool GuiCreate(ID3D11Device* device, ID3D11DeviceContext* context)
{
    g_device = device;
    g_ctx = context;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenScalerSettings";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"OpenScaler", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             100, 100, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;

    // 창 크기: 픽셀 단위 315 x 432 (배율 P를 곱한다)
    UINT dpi = GetDpiForWindow(g_hwnd);
    g_dpiScale = dpi / 96.0f;
    P = PixelScaleFor(g_dpiScale);
    RECT rc = { 0, 0, (LONG)(315 * P), (LONG)(432 * P) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

    // 작업 표시줄을 뺀 화면 가운데에 놓고, 화면보다 크면 높이를 줄인다 (넘치는 내용은 스크롤)
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& work = mi.rcWork;
    int w = std::min<int>(rc.right - rc.left, work.right - work.left);
    int h = std::min<int>(rc.bottom - rc.top, work.bottom - work.top);
    SetWindowPos(g_hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);

    // 스왑체인
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;
    RECT client;
    GetClientRect(g_hwnd, &client);
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = client.right;
    scd.Height = client.bottom;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device, g_hwnd, &scd, nullptr, nullptr, &g_swapChain))) return false;
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateRenderTarget();

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // 창 배치 파일(imgui.ini)을 만들지 않는다
    ApplyScale(g_dpiScale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(device, context);

    RefreshWindows();
    g_open = true;
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    return true;
}

void GuiDestroy()
{
    if (!g_hwnd) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_logo.Reset();
    g_rtv.Reset();
    g_swapChain.Reset();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_open = false;
}

void GuiRender(bool throttled)
{
    if (!g_open || g_minimized) return;
    auto now = Clock::now();
    if (throttled && now - g_lastRender < std::chrono::milliseconds(100)) return;
    g_lastRender = now;

    if (g_pendingDpi > 0 && g_pendingDpi != g_dpiScale) {
        ApplyScale(g_pendingDpi);
        ImGui_ImplDX11_InvalidateDeviceObjects();  // 다음 NewFrame에서 글꼴 텍스처를 다시 만든다
    }
    g_pendingDpi = 0;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();

    ID3D11RenderTargetView* rtv = g_rtv.Get();
    const float clear[4] = { 1, 1, 1, 1 };
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);  // 창 크기를 바꿀 때 백버퍼를 놓아 줄 수 있게
    g_swapChain->Present(throttled ? 0 : 1, 0);
}

bool GuiIsOpen() { return g_open; }
bool GuiIsVisible() { return g_open && !g_minimized; }
HWND GuiHwnd() { return g_hwnd; }
HWND GuiSelectedWindow() { return g_selected; }
