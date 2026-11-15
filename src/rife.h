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
        // 신경망 출력을 평면 5장으로 이어 붙인 그대로 둔다 (w*h*5). 다시 늘어놓지 않고 텍스처에 바로 올린다.
        // 0~3장: 시점 0.5에서 앞 프레임으로 x, y, 뒤 프레임으로 x, y. 4장: 앞 프레임의 비중 (0~1)
        std::vector<float> planes;
        double ms = 0;     // 입력 준비부터 결과 정리까지
        double runMs = 0;  // 신경망 실행만
    };

    explicit RifeEngine(std::wstring directory);
    ~RifeEngine();

    State GetState();
    std::string GetError();

    // 입력 크기를 정한다. 세션은 크기를 고정해 만들므로(그래야 빠르다) 크기가 바뀌면 새 세션을 만들고 한 번 돌려 둔다.
    // 몇 초 걸리며, 그동안은 이전 크기의 세션으로 계속 처리한다. 세션이 하나도 없을 때만 STATE_LOADING이다.
    void SetInputSize(int w, int h);
    // 지금 처리할 수 있는 입력 크기. 이 크기와 다른 요청은 버린다. 세션이 없으면 false
    bool ActiveSize(int& w, int& h);

    // 입력은 0~1 평면 3채널(w*h*3). 처리 중이면 가장 최근 요청만 남긴다.
    void Submit(long long seq, std::vector<float> first, std::vector<float> second, int w, int h);
    bool TakeResult(Result& out);
    // 다 쓴 결과의 버퍼를 돌려주면 다음 결과에 다시 쓴다 (큰 버퍼를 매번 새로 만들고 0으로 채우지 않도록).
    void Recycle(std::vector<float>&& planes);
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
    void* BuildSession(int w, int h);  // 실패하면 nullptr
    bool Infer(void* session, Request& request, Result& out);

    std::wstring m_directory;
    std::thread m_thread;
    std::thread m_builder;      // 새 크기의 세션을 뒤에서 만든다
    bool m_building = false;
    void* m_newSession = nullptr;  // 다 만든 새 세션. 작업 스레드가 바꿔 낀다
    int m_newW = 0, m_newH = 0;
    long long m_droppedSeq = -1;   // 크기가 맞지 않아 버린 마지막 요청
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
    std::vector<float> m_spare;
    bool m_hasResult = false;
    double m_averageMs = 0;

    void* m_env = nullptr;
    void* m_session = nullptr;
    void* m_memory = nullptr;
    void* m_appendDml = nullptr;
    void* m_ortModule = nullptr;
    void* m_dmlModule = nullptr;
};
