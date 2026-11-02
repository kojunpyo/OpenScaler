#pragma once

// RIFE 신경망 실행기: ONNX Runtime(DirectML)을 별도 스레드에서 돌려 시점 0.5의 움직임과 마스크를 구한다.
// 실행 파일 옆 rife 폴더의 DLL과 모델을 실행 중에 불러오므로, 파일이 없어도 프로그램은 그대로 동작한다.

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RifeEngine {
public:
    enum State { STATE_LOADING, STATE_READY, STATE_FAILED };

    struct Result {
        long long seq = 0;
        int w = 0, h = 0;
        std::vector<float> flow;  // 픽셀마다 4값: 시점 0.5에서 앞 프레임으로 (x, y), 뒤 프레임으로 (x, y)
        std::vector<float> mask;  // 픽셀마다 앞 프레임의 비중 (0~1)
        double ms = 0;
    };

    explicit RifeEngine(std::wstring directory);
    ~RifeEngine();

    State GetState();
    std::string GetError();

    // 입력 크기를 정한다. 같은 세션으로 다른 크기를 돌리면 3배 가까이 느려져(GTX 1060에서 측정)
    // 크기가 바뀌면 세션을 새로 만들고 한 번 돌려 둔다. 몇 초 걸리며 그동안 상태는 STATE_LOADING이다.
    void SetInputSize(int w, int h);

    // 입력은 0~1 평면 3채널(w*h*3). 처리 중이면 가장 최근 요청만 남긴다.
    void Submit(long long seq, std::vector<float> first, std::vector<float> second, int w, int h);
    bool TakeResult(Result& out);
    // 결과가 나올 때까지 기다린다 (자체 테스트용)
    bool RunBlocking(std::vector<float> first, std::vector<float> second, int w, int h, Result& out);
    double AverageMs();
    int ResultCount();  // 지금 입력 크기로 낸 결과 수

    static std::wstring DefaultDirectory();
    static bool FilesExist(const std::wstring& directory);

private:
    struct Request {
        long long seq = 0;
        std::vector<float> first, second;
        int w = 0, h = 0;
        bool valid = false;
    };

    void Worker();
    bool Load();
    bool PrepareSession(int w, int h);
    bool Infer(Request& request, Result& out);

    std::wstring m_directory;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    bool m_quit = false;
    State m_state = STATE_LOADING;
    std::string m_error;
    Request m_request;
    int m_wantW = 0, m_wantH = 0;
    int m_sessionW = 0, m_sessionH = 0;
    int m_resultCount = 0;
    Result m_result;
    bool m_hasResult = false;
    double m_averageMs = 0;

    void* m_env = nullptr;
    void* m_session = nullptr;
    void* m_memory = nullptr;
    void* m_appendDml = nullptr;
    void* m_ortModule = nullptr;
    void* m_dmlModule = nullptr;
};
