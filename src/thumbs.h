#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ThumbRequest {
    size_t generation = 0;
    int index = -1;
    std::wstring path;
};

struct ThumbResult {
    ThumbRequest request;
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;             // PBGRA pixels; the UI thread creates the GPU bitmap.
};

// One background decoder thread; pending work includes only the viewport and buffer rows.
class ThumbLoader {
public:
    ~ThumbLoader() { stop(); }
    void submit(HWND hwnd, UINT message, size_t generation, std::vector<ThumbRequest> requests);
    std::vector<ThumbResult> takeResults();
    void cancel(size_t generation);
    void stop();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_ = false;
    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    size_t generation_ = 0;
    ThumbRequest active_;
    std::vector<ThumbRequest> requests_;
    std::vector<ThumbResult> results_;
};
