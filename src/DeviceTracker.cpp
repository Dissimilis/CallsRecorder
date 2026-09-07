#include "DeviceTracker.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <vector>

// MinGW's endpointvolume.h only forward-declares IAudioMeterInformation;
// declare it ourselves (IID and vtable order from the Windows SDK).
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInfo : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
static const GUID IID_IAudioMeterInfo =
    {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

// Apps whose active mic session marks a call in progress. Dedicated apps
// first; browsers cover Google Meet & co.
static const wchar_t* kCallApps[] = {
    L"ms-teams.exe", L"msteams.exe", L"teams.exe", L"zoom.exe", L"skype.exe",
    L"discord.exe", L"slack.exe", L"webex.exe", L"ciscocollabhost.exe",
    L"telegram.exe", L"whatsapp.exe", L"signal.exe", L"viber.exe",
    L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe",
    L"vivaldi.exe", L"chromium.exe", L"arc.exe",
};

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Last resort when the process can't be opened: the session instance id is
// documented as opaque, but in practice embeds the exe path
// ("{...}|\Device\HarddiskVolume3\...\zoom.exe%b{...}"). Only used as a
// fallback, and only accepted if it yields a known call app.
static std::wstring ExeFromSessionId(IAudioSessionControl2* ctl2) {
    LPWSTR sid = nullptr;
    if (FAILED(ctl2->GetSessionInstanceIdentifier(&sid)) || !sid) return L"";
    std::wstring s = sid;
    CoTaskMemFree(sid);
    size_t end = s.find(L"%b");
    if (end == std::wstring::npos) end = s.size();
    size_t slash = s.rfind(L'\\', end);
    if (slash == std::wstring::npos) return L"";
    return ToLower(s.substr(slash + 1, end - slash - 1));
}

static std::wstring ExeNameForPid(DWORD pid) {
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        static std::mutex loggedMtx;   // Pick() and Probe() run on different threads
        static std::set<DWORD> logged; // one line per pid, not one per poll
        DWORD err = GetLastError();
        bool first;
        {
            std::lock_guard<std::mutex> lk(loggedMtx);
            first = logged.insert(pid).second;
        }
        if (first)
            LogLine(L"DeviceTracker: OpenProcess(%lu) failed err=%lu", pid, err);
        return L"";
    }
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = MAX_PATH * 2;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        name = ToLower(slash ? slash + 1 : path);
    }
    CloseHandle(h);
    return name;
}

static bool IsKnownCallApp(const std::wstring& exe) {
    for (const wchar_t* a : kCallApps)
        if (exe == a) return true;
    return false;
}

// A session belonging to some process, with its owning device and peak level.
struct SessionHit {
    std::wstring deviceId;
    std::wstring exe;
    float peak = -1;
};

// Enumerates active, non-system sessions on all active devices of a flow.
// matchExe: if non-empty, only that exe counts; otherwise any known call app.
// Returns hits in enumeration order.
static std::vector<SessionHit> ScanSessions(IMMDeviceEnumerator* en, EDataFlow flow,
                                            const std::wstring& matchExe, bool wantPeak) {
    std::vector<SessionHit> hits;
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return hits;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;

        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                int n = 0;
                sessions->GetCount(&n);
                for (int s = 0; s < n; s++) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(sessions->GetSession(s, &ctl))) continue;

                    AudioSessionState state = AudioSessionStateInactive;
                    ctl->GetState(&state);
                    if (state == AudioSessionStateActive) {
                        IAudioSessionControl2* ctl2 = nullptr;
                        if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctl2))) {
                            if (ctl2->IsSystemSoundsSession() != S_OK) {
                                DWORD pid = 0;
                                ctl2->GetProcessId(&pid);
                                std::wstring exe = ExeNameForPid(pid);
                                if (exe.empty()) {
                                    exe = ExeFromSessionId(ctl2);
                                    if (!IsKnownCallApp(exe)) exe.clear();
                                }
                                bool hit = matchExe.empty() ? IsKnownCallApp(exe) : (exe == matchExe);
                                if (hit) {
                                    SessionHit h;
                                    h.exe = exe;
                                    LPWSTR id = nullptr;
                                    if (SUCCEEDED(dev->GetId(&id))) {
                                        h.deviceId = id;
                                        CoTaskMemFree(id);
                                    }
                                    if (wantPeak) {
                                        IAudioMeterInfo* meter = nullptr;
                                        if (SUCCEEDED(ctl->QueryInterface(IID_IAudioMeterInfo, (void**)&meter))) {
                                            float p = 0;
                                            if (SUCCEEDED(meter->GetPeakValue(&p))) h.peak = p;
                                            meter->Release();
                                        }
                                    }
                                    hits.push_back(h);
                                }
                            }
                            ctl2->Release();
                        }
                    }
                    ctl->Release();
                }
                sessions->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    coll->Release();
    return hits;
}

static std::wstring DefaultCommDevice(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::wstring result;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eCommunications, &dev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            result = id;
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    return result;
}

static IMMDeviceEnumerator* MakeEnumerator() {
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) LogLine(L"DeviceTracker: enumerator failed hr=0x%08X", hr);
    return en;
}

DevicePick DeviceTracker::Pick() {
    DevicePick p;
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return p;

    // A call app actively using a mic is the strongest signal of a call.
    auto mics = ScanSessions(en, eCapture, L"", false);
    if (!mics.empty()) {
        p.tracked = true;
        p.micId = mics[0].deviceId;
        p.app = mics[0].exe;
        // Prefer the render device the same app is playing through.
        auto rends = ScanSessions(en, eRender, p.app, false);
        if (!rends.empty()) p.renderId = rends[0].deviceId;
        if (p.renderId.empty())
            p.renderId = DefaultCommDevice(en, eRender);
    } else {
        p.micId = DefaultCommDevice(en, eCapture);
        p.renderId = DefaultCommDevice(en, eRender);
    }
    en->Release();
    return p;
}

// --- Meeting window heuristics -------------------------------------------
// Only Zoom has a well-known in-meeting window class. Absence is treated as
// soft evidence by the detector, never as a hard stop.
struct WindowScan {
    std::wstring exe;
    bool found = false;
};

static BOOL CALLBACK FindMeetingWindow(HWND hwnd, LPARAM lp) {
    WindowScan* ws = (WindowScan*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"ZPContentViewWndClass") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (ExeNameForPid(pid) == ws->exe) {
        ws->found = true;
        return FALSE;
    }
    return TRUE;
}

static int MeetingWindowState(const std::wstring& exe) {
    if (exe != L"zoom.exe") return -1;
    WindowScan ws;
    ws.exe = exe;
    EnumWindows(FindMeetingWindow, (LPARAM)&ws);
    return ws.found ? 1 : 0;
}

CallProbe DeviceTracker::Probe() {
    CallProbe c;
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return c;
    c.valid = true;

    auto mics = ScanSessions(en, eCapture, L"", true);
    if (!mics.empty()) {
        c.micActive = true;
        c.app = mics[0].exe;
        for (auto& h : mics)
            if (h.exe == c.app) c.micPeak = std::max(c.micPeak, h.peak);

        auto rends = ScanSessions(en, eRender, c.app, true);
        c.renderActive = !rends.empty();
        for (auto& h : rends) c.renderPeak = std::max(c.renderPeak, h.peak);
        c.meetingWindow = MeetingWindowState(c.app);
    }
    en->Release();
    return c;
}
