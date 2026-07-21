#include "Recorder.h"
#include "AudioCapture.h"
#include "DeviceTracker.h"
#include "Mp3Writer.h"
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <algorithm>
#include <vector>

static const size_t kMaxLoopBacklogFrames = 48000; // 1 s
static const size_t kTrimLoopToFrames = 9600;      // 200 ms

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

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\%04u-%02u-%02u %02u-%02u-%02u.mp3",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    currentFile_ = folder + name;

    stopFlag_ = false;
    running_ = true;
    startTick_ = GetTickCount64();
    thread_ = std::thread(&Recorder::run, this, currentFile_);
    return true;
}

void Recorder::stop() {
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void Recorder::run(std::wstring path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) {
            LogLine(L"Recorder: device enumerator unavailable");
            running_ = false;
            CoUninitialize();
            return;
        }

        Mp3Writer writer;
        if (!writer.open(path)) {
            running_ = false;
            en->Release();
            CoUninitialize();
            return;
        }

        AudioCaptureStream mic, loop;
        DevicePick cur = DeviceTracker::Pick();
        bool micOk = OpenStream(en, mic, cur.micId, false);
        bool loopOk = OpenStream(en, loop, cur.renderId, true);
        LogLine(L"Recorder: start mic=%d loop=%d tracked=%d app=%ls",
                micOk, loopOk, cur.tracked, cur.app.empty() ? L"-" : cur.app.c_str());

        std::vector<int16_t> micRing, loopRing, mixBuf;
        int tick = 0;
        int micStarvedTicks = 0;
        bool writeFailed = false;

        while (!stopFlag_ && !writeFailed) {
            Sleep(10);
            bool err = false;
            if (micOk && !mic.pump(micRing)) { micOk = false; err = true; }
            if (loopOk && !loop.pump(loopRing)) { loopOk = false; err = true; }

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
                if (!writer.write(loopRing.data(), lf)) writeFailed = true;
                loopRing.clear();
                micRing.clear();
            }
            size_t frames = micRing.size() / 2;
            if (frames) {
                size_t lf = std::min(frames, loopRing.size() / 2);
                mixBuf.resize(frames * 2);
                for (size_t i = 0; i < frames * 2; i++) {
                    int v = micRing[i] + (i < lf * 2 ? loopRing[i] : 0);
                    mixBuf[i] = (int16_t)std::clamp(v, -32768, 32767);
                }
                if (!writer.write(mixBuf.data(), frames)) writeFailed = true;
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
                if (micChanged) micOk = OpenStream(en, mic, np.micId, false);
                if (loopChanged) loopOk = OpenStream(en, loop, np.renderId, true);
                cur = np;
            }
        }

        mic.shutdown();
        loop.shutdown();
        writer.close();
        en->Release();
    }
    running_ = false;
    CoUninitialize();
}
