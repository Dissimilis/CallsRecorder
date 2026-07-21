#include "Recorder.h"
#include "AudioCapture.h"
#include "DeviceTracker.h"
#include "Mp3Writer.h"
#include "Settings.h"
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <algorithm>
#include <vector>

static const size_t kMaxLoopBacklogFrames = 48000; // 1 s
static const size_t kTrimLoopToFrames = 9600;      // 200 ms
static const size_t kLevelBlockFrames = 2400;      // one waveform level per 50 ms

static bool OpenStream(IMMDeviceEnumerator* en, AudioCaptureStream& stream,
                       const std::wstring& id, bool loopback) {
    stream.shutdown();
    if (id.empty()) return false;
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDevice(id.c_str(), &dev))) return false;
    bool ok = stream.init(dev, loopback);
    dev->Release();
    return ok;
}

bool Recorder::start(const std::wstring& folder) {
    if (running_) return true;
    if (thread_.joinable()) thread_.join();

    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);

    stopFlag_ = false;
    running_ = true;
    startTick_ = GetTickCount64();
    levelSeq_ = 0;
    levelAccum_ = 0;
    levelFrames_ = 0;
    thread_ = std::thread(&Recorder::run, this, folder);
    return true;
}

void Recorder::stop() {
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

std::wstring Recorder::currentFile() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return currentFile_;
}

std::wstring Recorder::currentApp() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return currentApp_;
}

size_t Recorder::levelHistory(float* out, size_t maxN) const {
    unsigned seq = levelSeq_;
    size_t n = std::min((size_t)seq, std::min(maxN, kLevelCount));
    for (size_t i = 0; i < n; i++)
        out[i] = levels_[(seq - n + i) % kLevelCount];
    return n;
}

void Recorder::pushLevel(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        float v = (float)(samples[i] < 0 ? -samples[i] : samples[i]) / 32768.0f;
        if (v > levelAccum_) levelAccum_ = v;
    }
    levelFrames_ += count / 2;
    if (levelFrames_ >= kLevelBlockFrames) {
        unsigned seq = levelSeq_;
        levels_[seq % kLevelCount] = levelAccum_;
        levelSeq_ = seq + 1;
        levelAccum_ = 0;
        levelFrames_ = 0;
    }
}

// Strips ".exe" for use in a file name ("zoom.exe" -> "zoom").
static std::wstring AppBaseName(const std::wstring& exe) {
    size_t dot = exe.rfind(L'.');
    return dot == std::wstring::npos ? exe : exe.substr(0, dot);
}

void Recorder::run(std::wstring folder) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // An escaping exception would terminate the process and leave running_
    // stuck true; log it and let the recording die cleanly instead.
    try {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) {
            LogLine(L"Recorder: device enumerator unavailable");
            running_ = false;
            CoUninitialize();
            return;
        }

        bool split = Settings::GetSplitChannels();

        AudioCaptureStream mic, loop;
        DevicePick cur = DeviceTracker::Pick();
        bool micOk = OpenStream(en, mic, cur.micId, false);
        bool loopOk = OpenStream(en, loop, cur.renderId, true);

        // File name carries the call app when one was detected:
        // "2026-07-21 14-32-05 teams.mp3".
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[96];
        swprintf(name, 96, L"\\%04u-%02u-%02u %02u-%02u-%02u", st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::wstring base = folder + name;
        if (cur.tracked) base += L" " + AppBaseName(cur.app);
        // Timestamps have one-second resolution; never overwrite an existing file.
        std::wstring path = base + L".mp3";
        for (int n = 2; n < 100 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; n++)
            path = base + L" (" + std::to_wstring(n) + L").mp3";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) // 99 collisions: last resort
            path = base + L" " + std::to_wstring(GetTickCount64()) + L".mp3";
        {
            std::lock_guard<std::mutex> lk(mtx_);
            currentFile_ = path;
            currentApp_ = cur.tracked ? cur.app : L"";
        }

        Mp3Writer writer;
        if (!writer.open(path)) {
            // Shut streams down here: their destructors must not run after
            // CoUninitialize below.
            mic.shutdown();
            loop.shutdown();
            running_ = false;
            en->Release();
            CoUninitialize();
            return;
        }
        LogLine(L"Recorder: start mic=%d loop=%d tracked=%d app=%ls split=%d",
                micOk, loopOk, cur.tracked, cur.app.empty() ? L"-" : cur.app.c_str(), split);

        std::vector<int16_t> micRing, loopRing, mixBuf;
        int tick = 0;
        int micStarvedTicks = 0;
        bool writeFailed = false;

        auto mixFrame = [split](int16_t* out, const int16_t* m, const int16_t* l) {
            if (split) {
                out[0] = m ? (int16_t)(((int)m[0] + m[1]) / 2) : 0;
                out[1] = l ? (int16_t)(((int)l[0] + l[1]) / 2) : 0;
            } else {
                int a = (m ? m[0] : 0) + (l ? l[0] : 0);
                int b = (m ? m[1] : 0) + (l ? l[1] : 0);
                out[0] = (int16_t)std::clamp(a, -32768, 32767);
                out[1] = (int16_t)std::clamp(b, -32768, 32767);
            }
        };

        while (!stopFlag_ && !writeFailed) {
            Sleep(10);
            bool err = false, micDisc = false;
            if (micOk && !mic.pump(micRing, &micDisc)) { micOk = false; err = true; }
            if (loopOk && !loop.pump(loopRing)) { loopOk = false; err = true; }

            // The mic drives the timeline, so if it dropped data the queued
            // loopback audio now leads it. Discard the backlog to stay in sync.
            if (micDisc && !loopRing.empty()) {
                LogLine(L"Recorder: mic glitch, resyncing (%zu loopback frames dropped)",
                        loopRing.size() / 2);
                loopRing.clear();
            }

            // A stream can be "open" yet deliver nothing (e.g. an idle
            // Bluetooth hands-free endpoint). Treat a mic silent for >1 s as
            // absent so loopback still gets recorded.
            micStarvedTicks = micRing.empty() ? micStarvedTicks + 1 : 0;
            bool micAlive = micOk && micStarvedTicks < 100;

            // Mic drives the timeline; pad the other side with silence.
            // With no working mic, loopback drives instead so the call side
            // still gets recorded.
            if (!micAlive && !loopRing.empty()) {
                size_t lf = loopRing.size() / 2;
                mixBuf.resize(lf * 2);
                for (size_t i = 0; i < lf; i++)
                    mixFrame(&mixBuf[i * 2], nullptr, &loopRing[i * 2]);
                if (!writer.write(mixBuf.data(), lf)) writeFailed = true;
                pushLevel(mixBuf.data(), mixBuf.size());
                loopRing.clear();
                micRing.clear();
            }
            size_t frames = micRing.size() / 2;
            if (frames) {
                size_t lf = std::min(frames, loopRing.size() / 2);
                mixBuf.resize(frames * 2);
                for (size_t i = 0; i < frames; i++)
                    mixFrame(&mixBuf[i * 2], &micRing[i * 2],
                             i < lf ? &loopRing[i * 2] : nullptr);
                if (!writer.write(mixBuf.data(), frames)) writeFailed = true;
                pushLevel(mixBuf.data(), mixBuf.size());
                micRing.clear();
                loopRing.erase(loopRing.begin(), loopRing.begin() + lf * 2);
            }
            // Clock drift guard: never let loopback lag more than 1 s behind.
            if (loopRing.size() / 2 > kMaxLoopBacklogFrames)
                loopRing.erase(loopRing.begin(),
                               loopRing.end() - kTrimLoopToFrames * 2);

            if (++tick >= 200 || err) { // every 2 s, or right after a device died
                tick = 0;
                if (err) Sleep(300);
                DevicePick np = DeviceTracker::Pick();
                bool micChanged = np.micId != cur.micId || !micOk;
                bool loopChanged = np.renderId != cur.renderId || !loopOk;
                if (micChanged || loopChanged)
                    LogLine(L"Recorder: device change (tracked=%d app=%ls)",
                            np.tracked, np.app.empty() ? L"-" : np.app.c_str());
                // Drop frames queued from the old endpoints so we never mix
                // stale old-device audio against the new streams.
                if (micChanged) { micRing.clear(); micOk = OpenStream(en, mic, np.micId, false); }
                if (loopChanged) { loopRing.clear(); loopOk = OpenStream(en, loop, np.renderId, true); }
                if (np.tracked) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    currentApp_ = np.app;
                }
                cur = np;
            }
        }

        // Final drain: flush audio still queued in WASAPI and our rings so a
        // stop doesn't drop the tail (up to ~1 s when the mic was starving).
        if (!writeFailed) {
            if (micOk) mic.pump(micRing);
            if (loopOk) loop.pump(loopRing);
            size_t mf = micRing.size() / 2, lf = loopRing.size() / 2;
            size_t total = std::max(mf, lf);
            if (total) {
                mixBuf.resize(total * 2);
                for (size_t i = 0; i < total; i++)
                    mixFrame(&mixBuf[i * 2], i < mf ? &micRing[i * 2] : nullptr,
                             i < lf ? &loopRing[i * 2] : nullptr);
                writer.write(mixBuf.data(), total);
            }
        }

        mic.shutdown();
        loop.shutdown();
        writer.close();
        en->Release();
    } catch (const std::exception& e) {
        LogLine(L"Recorder: fatal exception: %hs", e.what());
    } catch (...) {
        LogLine(L"Recorder: fatal unknown exception");
    }
    running_ = false;
    CoUninitialize();
}
