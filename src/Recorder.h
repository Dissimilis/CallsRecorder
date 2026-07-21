#pragma once
#include "common.h"
#include <atomic>
#include <mutex>
#include <thread>

// Orchestrates a recording: device pick -> two capture streams -> mix -> MP3.
// Runs on its own thread; re-picks devices every 2 s and follows switches.
class Recorder {
public:
    bool start(const std::wstring& folder);
    void stop();
    bool recording() const { return running_; }
    ULONGLONG elapsedMs() const { return recording() ? GetTickCount64() - startTick_ : 0; }
    std::wstring currentFile() const;
    std::wstring currentApp() const; // call app exe the recording is tracking ("" = none)

    // Copies up to maxN most-recent audio levels (peak per ~50 ms, 0..1),
    // oldest first. Returns the number copied. For the debug window waveform.
    size_t levelHistory(float* out, size_t maxN) const;

    ~Recorder() { stop(); }

    static const size_t kLevelCount = 256;

private:
    void run(std::wstring folder);
    void pushLevel(const int16_t* samples, size_t count);

    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> running_{false};
    ULONGLONG startTick_ = 0;
    mutable std::mutex mtx_; // guards currentFile_/currentApp_
    std::wstring currentFile_;
    std::wstring currentApp_;
    std::thread thread_;

    std::atomic<float> levels_[kLevelCount] = {};
    std::atomic<unsigned> levelSeq_{0};
    float levelAccum_ = 0;
    size_t levelFrames_ = 0;
};
