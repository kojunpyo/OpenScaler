#include "rife.h"

#include <windows.h>

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
        if (m_state != STATE_FAILED) m_state = STATE_LOADING;
    }
    m_wake.notify_all();
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

bool RifeEngine::RunBlocking(std::vector<float> first, std::vector<float> second, int w, int h, Result& out)
{
    static long long nextSeq = 1LL << 40;
    const long long seq = nextSeq++;
    Submit(seq, std::move(first), std::move(second), w, h);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&] { return m_state == STATE_FAILED || (m_hasResult && m_result.seq == seq); });
    if (m_state == STATE_FAILED) return false;
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

bool RifeEngine::PrepareSession(int w, int h)
{
    std::string error;
    auto fail = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
        m_state = STATE_FAILED;
        m_done.notify_all();
        return false;
    };

    if (m_session) {
        g_ort->ReleaseSession(static_cast<OrtSession*>(m_session));
        m_session = nullptr;
    }

    OrtSessionOptions* options = nullptr;
    if (Failed(g_ort->CreateSessionOptions(&options), error)) return fail();
    // DirectML이 요구하는 설정
    auto appendDml = reinterpret_cast<AppendDmlFn>(m_appendDml);
    bool bad = Failed(g_ort->DisableMemPattern(options), error) ||
               Failed(g_ort->SetSessionExecutionMode(options, ORT_SEQUENTIAL), error) ||
               Failed(appendDml(options, 0), error);
    OrtSession* session = nullptr;
    if (!bad) {
        bad = Failed(g_ort->CreateSession(static_cast<OrtEnv*>(m_env), (m_directory + L"\\rife.onnx").c_str(), options, &session), error);
    }
    g_ort->ReleaseSessionOptions(options);
    if (bad) return fail();
    m_session = session;

    // 첫 실행 때 셰이더를 컴파일해 몇 초 걸리므로 시작 전에 한 번 돌려 둔다.
    Request warm;
    warm.w = w;
    warm.h = h;
    warm.first.assign((size_t)w * h * 3, 0.5f);
    warm.second = warm.first;
    Result ignored;
    if (!Infer(warm, ignored)) return false;

    m_sessionW = w;
    m_sessionH = h;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_wantW == w && m_wantH == h) m_state = STATE_READY;
    m_averageMs = 0;
    m_resultCount = 0;
    return true;
}

bool RifeEngine::Infer(Request& request, Result& out)
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
    const size_t count = (size_t)request.w * request.h * 3;
    auto memory = static_cast<OrtMemoryInfo*>(m_memory);
    OrtValue* inputs[2] = {};
    OrtValue* outputs[2] = {};
    const char* inputNames[2] = { "img0", "img1" };
    const char* outputNames[2] = { "flow", "mask" };

    bool bad = Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, request.first.data(), count * sizeof(float), shape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]), error) ||
               Failed(g_ort->CreateTensorWithDataAsOrtValue(memory, request.second.data(), count * sizeof(float), shape, 4,
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]), error) ||
               Failed(g_ort->Run(static_cast<OrtSession*>(m_session), nullptr, inputNames, inputs, 2, outputNames, 2, outputs), error);

    if (!bad) {
        float* flow = nullptr;
        float* mask = nullptr;
        bad = Failed(g_ort->GetTensorMutableData(outputs[0], reinterpret_cast<void**>(&flow)), error) ||
              Failed(g_ort->GetTensorMutableData(outputs[1], reinterpret_cast<void**>(&mask)), error);
        if (!bad) {
            // 평면 형식을 텍스처에 바로 올릴 수 있는 형식으로 바꾼다.
            const size_t pixels = (size_t)request.w * request.h;
            out.seq = request.seq;
            out.w = request.w;
            out.h = request.h;
            out.flow.resize(pixels * 4);
            for (size_t i = 0; i < pixels; i++)
                for (size_t c = 0; c < 4; c++) out.flow[i * 4 + c] = flow[c * pixels + i];
            out.mask.assign(mask, mask + pixels);
        }
    }
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
        int wantW = 0, wantH = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_quit || m_request.valid || (m_wantW && (m_wantW != m_sessionW || m_wantH != m_sessionH)); });
            if (m_quit) break;
            wantW = m_wantW;
            wantH = m_wantH;
            if (wantW == m_sessionW && wantH == m_sessionH) {
                request = std::move(m_request);
                m_request = Request();
            } else {
                m_request = Request();  // 크기가 맞지 않는 요청은 버린다
            }
        }
        if (wantW != m_sessionW || wantH != m_sessionH) {
            if (!PrepareSession(wantW, wantH)) break;
            m_done.notify_all();
            continue;
        }
        if (!request.valid || request.w != m_sessionW || request.h != m_sessionH) continue;
        Result result;
        if (!Infer(request, result)) break;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_averageMs = m_averageMs <= 0 ? result.ms : m_averageMs * 0.9 + result.ms * 0.1;
            m_resultCount++;
            m_result = std::move(result);
            m_hasResult = true;
        }
        m_done.notify_all();
    }

    if (m_memory) g_ort->ReleaseMemoryInfo(static_cast<OrtMemoryInfo*>(m_memory));
    if (m_session) g_ort->ReleaseSession(static_cast<OrtSession*>(m_session));
    if (m_env) g_ort->ReleaseEnv(static_cast<OrtEnv*>(m_env));
    // DLL은 프로그램이 끝날 때까지 내려놓지 않는다 (다시 켤 때 재사용하고 종료 순서 문제를 피한다).
}
