#include "rife.h"

#include <windows.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstring>

#include "onnxruntime_c_api.h"

namespace {

using AppendDmlFn = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();

const OrtApi* g_ort = nullptr;

// 오류가 있으면 메시지를 꺼내고 true를 돌려준다.
bool Failed(OrtStatus* status, std::string& error)
{
    if (!status) return false;
    error = g_ort->GetErrorMessage(status);
    g_ort->ReleaseStatus(status);
    return true;
}

// DirectML의 장치 번호는 DXGI 어댑터 순서다. 0번은 보통 화면이 연결된 GPU라서,
// 내장 GPU와 외장 GPU가 함께 있는 노트북에서는 느린 내장 GPU가 될 수 있다. 성능 우선 GPU의 번호를 찾는다.
int HighPerformanceAdapter()
{
    IDXGIFactory6* factory6 = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6)))) return 0;
    int index = 0;
    IDXGIAdapter1* best = nullptr;
    if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&best)))) {
        DXGI_ADAPTER_DESC1 want = {};
        best->GetDesc1(&want);
        best->Release();
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory6->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            adapter->Release();
            if (desc.AdapterLuid.LowPart == want.AdapterLuid.LowPart && desc.AdapterLuid.HighPart == want.AdapterLuid.HighPart) {
                index = (int)i;
                break;
            }
        }
    }
    factory6->Release();
    return index;
}

}  // namespace

std::wstring RifeEngine::DefaultDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"rife";
}

bool RifeEngine::FilesExist(const std::wstring& directory)
{
    for (const wchar_t* name : { L"\\onnxruntime.dll", L"\\DirectML.dll", L"\\rife.onnx" })
        if (GetFileAttributesW((directory + name).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return true;
}

RifeEngine::RifeEngine(std::wstring directory) : m_directory(std::move(directory))
{
    m_thread = std::thread([this] { Worker(); });
}

RifeEngine::~RifeEngine()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

RifeEngine::State RifeEngine::GetState()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::string RifeEngine::GetError()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

double RifeEngine::AverageMs()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_averageMs;
}

int RifeEngine::ResultCount()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resultCount;
}

void RifeEngine::SetInputSize(int w, int h)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_wantW == w && m_wantH == h) return;
        m_wantW = w;
        m_wantH = h;
    }
    m_wake.notify_all();
}

bool RifeEngine::ActiveSize(int& w, int& h)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    w = m_sessionW;
    h = m_sessionH;
    return m_session != nullptr;
}

void RifeEngine::Submit(long long seq, std::vector<float> first, std::vector<float> second, int w, int h)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request.seq = seq;
        m_request.first = std::move(first);
        m_request.second = std::move(second);
        m_request.w = w;
        m_request.h = h;
        m_request.valid = true;
    }
    m_wake.notify_all();
}

bool RifeEngine::TakeResult(Result& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasResult) return false;
    out = std::move(m_result);
    m_hasResult = false;
    return true;
}

void RifeEngine::Recycle(std::vector<float>&& planes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_spare = std::move(planes);
}

bool RifeEngine::RunBlocking(std::vector<float> first, std::vector<float> second, int w, int h, Result& out)
{
    static long long nextSeq = 1LL << 40;
    const long long seq = nextSeq++;
    Submit(seq, std::move(first), std::move(second), w, h);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&] { return m_state == STATE_FAILED || m_droppedSeq == seq || (m_hasResult && m_result.seq == seq); });
    if (m_state == STATE_FAILED || m_droppedSeq == seq) return false;
    out = std::move(m_result);
    m_hasResult = false;
    return true;
}

bool RifeEngine::Load()
{
    std::string error;
    auto fail = [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = message;
        m_state = STATE_FAILED;
        m_done.notify_all();
        return false;
    };

    if (!FilesExist(m_directory)) return fail("rife 폴더에 파일이 없어요");

    // Windows 폴더에 다른 버전의 onnxruntime.dll이 있을 수 있으므로 반드시 전체 경로로 불러온다.
    HMODULE dml = LoadLibraryExW((m_directory + L"\\DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dml) return fail("DirectML.dll을 불러오지 못했어요");
    HMODULE ort = LoadLibraryExW((m_directory + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ort) return fail("onnxruntime.dll을 불러오지 못했어요");
    m_dmlModule = dml;
    m_ortModule = ort;

    auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort, "OrtGetApiBase"));
    auto appendDml = reinterpret_cast<AppendDmlFn>(GetProcAddress(ort, "OrtSessionOptionsAppendExecutionProvider_DML"));
    if (!getApiBase || !appendDml) return fail("이 onnxruntime.dll은 DirectML을 지원하지 않아요");
    g_ort = getApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) return fail("onnxruntime.dll 버전이 맞지 않아요");

    OrtEnv* env = nullptr;
    if (Failed(g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "OpenScaler", &env), error)) return fail(error);
    m_env = env;

    m_appendDml = reinterpret_cast<void*>(appendDml);

    OrtMemoryInfo* memory = nullptr;
    if (Failed(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory), error)) return fail(error);
    m_memory = memory;
    return true;
}

void* RifeEngine::BuildSession(int w, int h)
{
    std::string error;
    auto fail = [&]() -> void* {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
        m_state = STATE_FAILED;
        m_done.notify_all();
        return nullptr;
    };

    OrtSessionOptions* options = nullptr;
    if (Failed(g_ort->CreateSessionOptions(&options), error)) return fail();
    // DirectML이 요구하는 설정
    auto appendDml = reinterpret_cast<AppendDmlFn>(m_appendDml);
    bool bad = Failed(g_ort->DisableMemPattern(options), error) ||
               Failed(g_ort->SetSessionExecutionMode(options, ORT_SEQUENTIAL), error) ||
               Failed(appendDml(options, HighPerformanceAdapter()), error);
    // 모델은 크기가 바뀌어도 되게 내보냈지만, 세션은 크기마다 새로 만드므로 크기를 고정해 준다.
    // 크기가 정해지면 크기 계산이 미리 끝나고 DirectML이 신경망 전체를 한 덩어리로 컴파일한다.
    if (!bad)
        bad = Failed(g_ort->AddFreeDimensionOverrideByName(options, "64*h", h), error) ||
              Failed(g_ort->AddFreeDimensionOverrideByName(options, "64*w", w), error);
    // 연산별 시간을 보려면 환경 변수 OPENSCALER_RIFE_PROFILE에 파일 이름 앞부분을 준다 (세션을 닫을 때 JSON으로 저장).
    wchar_t profile[MAX_PATH];
    if (!bad && GetEnvironmentVariableW(L"OPENSCALER_RIFE_PROFILE", profile, MAX_PATH) > 0)
        bad = Failed(g_ort->EnableProfiling(options, profile), error);
    OrtSession* session = nullptr;
    if (!bad) {
        bad = Failed(g_ort->CreateSession(static_cast<OrtEnv*>(m_env), (m_directory + L"\\rife.onnx").c_str(), options, &session), error);
    }
    g_ort->ReleaseSessionOptions(options);
    if (bad) return fail();

    // 첫 실행 때 셰이더를 컴파일해 몇 초 걸리므로 시작 전에 한 번 돌려 둔다.
    Request warm;
    warm.w = w;
    warm.h = h;
    warm.first.assign((size_t)w * h * 3, 0.5f);
    warm.second = warm.first;
    Result ignored;
    if (!Infer(session, warm, ignored)) {
        g_ort->ReleaseSession(session);
        return nullptr;
    }
    return session;
}

bool RifeEngine::Infer(void* session, Request& request, Result& out)
{
    auto start = std::chrono::steady_clock::now();
    std::string error;
    auto fail = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
        m_state = STATE_FAILED;
        m_done.notify_all();
        return false;
    };

    const int64_t shape[4] = { 1, 3, request.h, request.w };
    const int64_t flowShape[4] = { 1, 4, request.h, request.w };
    const int64_t maskShape[4] = { 1, 1, request.h, request.w };
    const size_t pixels = (size_t)request.w * request.h;
    const size_t count = pixels * 3;
    // 출력을 미리 만든 버퍼에 바로 받는다. 움직임 4장 뒤에 마스크 1장이 이어진다.
    out.planes.resize(pixels * 5);
    auto memory = static_cast<OrtMemoryInfo*>(m_memory);
    OrtValue* inputs[2] = {};
    OrtValue* outputs[2] = {};
    const char* inputNames[2] = { "img0", "img1" };
    const char* outputNames[2] = { "flow", "mask" };

    bool bad = Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, request.first.data(), count * sizeof(float), shape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]), error) ||
               Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, request.second.data(), count * sizeof(float), shape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]), error) ||
               Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, out.planes.data(), pixels * 4 * sizeof(float), flowShape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputs[0]), error) ||
               Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, out.planes.data() + pixels * 4, pixels * sizeof(float), maskShape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputs[1]), error);
    auto runStart = std::chrono::steady_clock::now();
    if (!bad) bad = Failed(g_ort->Run(static_cast<OrtSession*>(session), nullptr, inputNames, inputs, 2, outputNames, 2, outputs), error);
    out.runMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();

    out.seq = request.seq;
    out.w = request.w;
    out.h = request.h;
    for (OrtValue* v : inputs) if (v) g_ort->ReleaseValue(v);
    for (OrtValue* v : outputs) if (v) g_ort->ReleaseValue(v);
    if (bad) return fail();

    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

void RifeEngine::Worker()
{
    if (!Load()) return;
    m_done.notify_all();

    while (true) {
        Request request;
        int buildW = 0, buildH = 0;
        void* retired = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto needBuild = [&] { return m_wantW && !m_building && (m_wantW != m_sessionW || m_wantH != m_sessionH); };
            m_wake.wait(lock, [&] { return m_quit || m_request.valid || m_newSession || m_state == STATE_FAILED || needBuild(); });
            if (m_quit || m_state == STATE_FAILED) break;
            if (m_newSession) {
                // 새 세션으로 바꿔 낀다. 시간과 결과 수는 크기마다 새로 센다.
                retired = m_session;
                m_session = m_newSession;
                m_sessionW = m_newW;
                m_sessionH = m_newH;
                m_newSession = nullptr;
                m_building = false;
                m_state = STATE_READY;
                m_averageMs = 0;
                m_resultCount = 0;
            }
            if (needBuild()) {
                m_building = true;
                buildW = m_wantW;
                buildH = m_wantH;
            }
            if (m_request.valid) {
                if (m_request.w == m_sessionW && m_request.h == m_sessionH && m_session) {
                    request = std::move(m_request);
                } else {
                    m_droppedSeq = m_request.seq;  // 크기가 맞지 않는 요청은 버린다
                    m_done.notify_all();
                }
                m_request = Request();
            }
        }
        if (retired) {
            if (m_builder.joinable()) m_builder.join();
            g_ort->ReleaseSession(static_cast<OrtSession*>(retired));
            m_done.notify_all();
        }
        if (buildW) {
            if (m_builder.joinable()) m_builder.join();
            m_builder = std::thread([this, buildW, buildH] {
                void* session = BuildSession(buildW, buildH);
                if (!session) {
                    m_wake.notify_all();  // 실패 상태를 작업 스레드에 알린다
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_newSession = session;
                    m_newW = buildW;
                    m_newH = buildH;
                }
                m_wake.notify_all();
            });
        }
        if (!request.valid) continue;
        Result result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            result.planes = std::move(m_spare);
        }
        if (!Infer(m_session, request, result)) break;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_averageMs = m_averageMs <= 0 ? result.ms : m_averageMs * 0.9 + result.ms * 0.1;
            m_resultCount++;
            m_result = std::move(result);
            m_hasResult = true;
        }
        m_done.notify_all();
    }

    if (m_builder.joinable()) m_builder.join();
    if (m_newSession) g_ort->ReleaseSession(static_cast<OrtSession*>(m_newSession));
    if (m_memory) g_ort->ReleaseMemoryInfo(static_cast<OrtMemoryInfo*>(m_memory));
    if (m_session) g_ort->ReleaseSession(static_cast<OrtSession*>(m_session));
    if (m_env) g_ort->ReleaseEnv(static_cast<OrtEnv*>(m_env));
    // DLL은 프로그램이 끝날 때까지 내려놓지 않는다 (다시 켤 때 재사용하고 종료 순서 문제를 피한다).
}
