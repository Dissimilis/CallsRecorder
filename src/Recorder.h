#pragma once
#include "common.h"
#include <atomic>
#include <thread>

// Orchestrates a recording: device pick -> two capture streams -> mix -> MP3.
// Runs on its own thread; re-picks devices every 2 s and follows switches.
class Recorder {
public:
    bool start(const std::wstring& folder);
    void stop();
    bool recording() const { return running_; }
    ULONGLONG elapsedMs() const { return recording() ? GetTickCount64() - startTick_ : 0; }
    std::wstring currentFile() const { return currentFile_; }
    ~Recorder() { stop(); }

private:
    void run(std::wstring path);

    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> running_{false};
    ULONGLONG startTick_ = 0;
    std::wstring currentFile_;
    std::thread thread_;
};
