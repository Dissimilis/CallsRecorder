#include "DeviceTracker.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <algorithm>
#include <vector>

// Apps whose active mic session marks a call in progress. Dedicated apps
// first; browsers cover Google Meet & co.
static const wchar_t* kCallApps[] = {
    L"ms-teams.exe", L"teams.exe", L"zoom.exe", L"skype.exe", L"discord.exe",
    L"slack.exe", L"webex.exe", L"ciscocollabhost.exe", L"telegram.exe",
    L"whatsapp.exe", L"signal.exe", L"viber.exe",
    L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe",
    L"vivaldi.exe",
};

static std::wstring ExeNameForPid(DWORD pid) {
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = MAX_PATH * 2;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        name = slash ? slash + 1 : path;
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    }
    CloseHandle(h);
    return name;
}

static bool IsKnownCallApp(const std::wstring& exe) {
    for (const wchar_t* a : kCallApps)
        if (exe == a) return true;
    return false;
}

// Finds a device (of the given flow) that has an active audio session from a
// call app. matchExe: if non-empty, only that exe counts (used to find the
// render device belonging to the app found on the mic side).
static std::wstring FindCallSessionDevice(IMMDeviceEnumerator* en, EDataFlow flow,
                                          const std::wstring& matchExe, std::wstring* foundExe) {
    std::wstring result;
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return result;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count && result.empty(); i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;

        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                int n = 0;
                sessions->GetCount(&n);
                for (int s = 0; s < n && result.empty(); s++) {
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
                                bool hit = matchExe.empty() ? IsKnownCallApp(exe) : (exe == matchExe);
                                if (hit) {
                                    LPWSTR id = nullptr;
                                    if (SUCCEEDED(dev->GetId(&id))) {
                                        result = id;
                                        CoTaskMemFree(id);
                                        if (foundExe) *foundExe = exe;
                                    }
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
    return result;
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

DevicePick DeviceTracker::Pick() {
    DevicePick p;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) {
        LogLine(L"DeviceTracker: enumerator failed hr=0x%08X", hr);
        return p;
    }

    // A call app actively using a mic is the strongest signal of a call.
    p.micId = FindCallSessionDevice(en, eCapture, L"", &p.app);
    if (!p.micId.empty()) {
        p.tracked = true;
        // Prefer the render device the same app is playing through.
        p.renderId = FindCallSessionDevice(en, eRender, p.app, nullptr);
        if (p.renderId.empty())
            p.renderId = DefaultCommDevice(en, eRender);
    } else {
        p.micId = DefaultCommDevice(en, eCapture);
        p.renderId = DefaultCommDevice(en, eRender);
    }
    en->Release();
    return p;
}
