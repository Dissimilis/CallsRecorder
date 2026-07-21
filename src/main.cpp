#include "common.h"
#include "Recorder.h"
#include "DeviceTracker.h"
#include "DebugWindow.h"
#include "Settings.h"
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <atomic>
#include <thread>
#include <cmath>

static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT WM_AUTOCMD = WM_APP + 2; // wParam: 1 = auto-start, 0 = auto-stop
static const UINT kTimerId = 1;
enum MenuId {
    kMenuStatus = 1, kMenuToggle, kMenuAuto, kMenuSplit, kMenuStartup,
    kMenuOpenFolder, kMenuChooseFolder, kMenuDebug, kMenuExit
};

static HWND g_hwnd;
static HINSTANCE g_inst;
static Recorder g_recorder;
static HICON g_iconIdle, g_iconRec;
static UINT g_taskbarCreatedMsg;

// Auto-record watcher state
static std::thread g_watcher;
static std::atomic<bool> g_watcherStop{false};
static std::atomic<bool> g_autoEnabled{false};
static std::atomic<bool> g_autoStartedRec{false};
// After a stop, wait until no call is detected before auto-starting again,
// so stopping manually mid-call doesn't immediately restart the recording.
static std::atomic<bool> g_rearm{false};

// 32x32 icon drawn in code: hollow ring when idle, solid red dot when recording.
static HICON MakeDotIcon(COLORREF color, bool solid) {
    const int S = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;

    BYTE cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = x - 15.5f, dy = y - 15.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 0;
            if (solid) {
                a = 12.5f - d; // filled circle r=12.5
            } else {
                a = 12.5f - d;
                float inner = d - 7.0f; // hollow: cut circle r=7
                if (inner < a) a = inner;
            }
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            BYTE al = (BYTE)(a * 255);
            px[y * S + x] = ((DWORD)al << 24) |
                            ((DWORD)(cr * al / 255) << 16) |
                            ((DWORD)(cg * al / 255) << 8) |
                            (DWORD)(cb * al / 255);
        }
    }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, bmp};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(bmp);
    return icon;
}

static void FormatElapsed(wchar_t* buf, size_t n) {
    ULONGLONG s = g_recorder.elapsedMs() / 1000;
    if (s >= 3600)
        swprintf(buf, n, L"%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    else
        swprintf(buf, n, L"%02llu:%02llu", s / 60, s % 60);
}

static void UpdateTrayIcon(bool add) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    bool rec = g_recorder.recording();
    nid.hIcon = rec ? g_iconRec : g_iconIdle;
    if (rec) {
        wchar_t t[32];
        FormatElapsed(t, 32);
        swprintf(nid.szTip, 128, L"CallsRecorder — recording %ls", t);
    } else {
        wcscpy(nid.szTip, L"CallsRecorder — idle");
    }
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
}

static void StartRecording(bool autoStarted) {
    if (g_recorder.recording()) return;
    if (g_recorder.start(Settings::GetStorageFolder())) {
        g_autoStartedRec = autoStarted;
        SetTimer(g_hwnd, kTimerId, 1000, nullptr);
        UpdateTrayIcon(false);
        LogLine(L"UI: recording started (%ls)", autoStarted ? L"auto" : L"manual");
    }
}

static void StopRecording(bool autoStopped) {
    if (!g_recorder.recording()) return;
    g_recorder.stop();
    g_rearm = true; // don't auto-restart until the current call ends
    KillTimer(g_hwnd, kTimerId);
    UpdateTrayIcon(false);
    LogLine(L"UI: recording stopped (%ls)", autoStopped ? L"auto" : L"manual");
}

static void ChooseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose folder for call recordings");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                Settings::SetStorageFolder(path);
                LogLine(L"UI: storage folder -> %ls", path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static void OpenFolder() {
    std::wstring folder = Settings::GetStorageFolder();
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool GetStartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"CallsRecorder",
                        RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS;
}

static void SetStartWithWindows(bool on) {
    if (on) {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"CallsRecorder", REG_SZ,
                        v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"CallsRecorder");
    }
    LogLine(L"UI: start with Windows -> %d", on);
}

// Polls for call-app audio sessions and asks the UI thread to start/stop.
static void AutoWatcher() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int missCount = 0;
    while (!g_watcherStop) {
        for (int i = 0; i < 30 && !g_watcherStop; i++) Sleep(100);
        if (g_watcherStop) break;
        if (!g_autoEnabled) { missCount = 0; continue; }

        DevicePick p = DeviceTracker::Pick();
        if (!g_recorder.recording()) {
            missCount = 0;
            if (!p.tracked)
                g_rearm = false;
            else if (!g_rearm)
                PostMessageW(g_hwnd, WM_AUTOCMD, 1, 0);
        } else if (g_autoStartedRec) {
            if (!p.tracked) {
                if (++missCount >= 2) { // ~6 s of no call before stopping
                    missCount = 0;
                    PostMessageW(g_hwnd, WM_AUTOCMD, 0, 0);
                }
            } else {
                missCount = 0;
            }
        }
    }
    CoUninitialize();
}

static void DoCommand(UINT cmd) {
    switch (cmd) {
        case kMenuToggle:
            if (g_recorder.recording()) StopRecording(false);
            else StartRecording(false);
            break;
        case kMenuAuto: {
            bool on = !Settings::GetAutoRecord();
            Settings::SetAutoRecord(on);
            g_autoEnabled = on;
            g_rearm = false;
            LogLine(L"UI: auto-record -> %d", on);
            break;
        }
        case kMenuSplit: {
            bool on = !Settings::GetSplitChannels();
            Settings::SetSplitChannels(on);
            LogLine(L"UI: split channels -> %d (applies to next recording)", on);
            break;
        }
        case kMenuStartup:
            SetStartWithWindows(!GetStartWithWindows());
            break;
        case kMenuOpenFolder: OpenFolder(); break;
        case kMenuChooseFolder: ChooseFolder(); break;
        case kMenuDebug: DebugWindow_Show(g_inst, &g_recorder); break;
        case kMenuExit:
            StopRecording(false);
            DestroyWindow(g_hwnd);
            break;
    }
}

static void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (g_recorder.recording()) {
        wchar_t t[32], status[80];
        FormatElapsed(t, 32);
        std::wstring app = g_recorder.currentApp();
        swprintf(status, 80, L"● Recording  %ls%ls%ls", t,
                 app.empty() ? L"" : L"  ", app.c_str());
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status);
        AppendMenuW(menu, MF_STRING, kMenuToggle, L"Stop recording");
    } else {
        AppendMenuW(menu, MF_STRING, kMenuToggle, L"Start recording");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (Settings::GetAutoRecord() ? MF_CHECKED : 0),
                kMenuAuto, L"Auto-record calls");
    AppendMenuW(menu, MF_STRING | (Settings::GetSplitChannels() ? MF_CHECKED : 0),
                kMenuSplit, L"Separate channels (mic left, call right)");
    AppendMenuW(menu, MF_STRING | (GetStartWithWindows() ? MF_CHECKED : 0),
                kMenuStartup, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Open recordings folder");
    AppendMenuW(menu, MF_STRING, kMenuChooseFolder, L"Choose storage folder…");
    AppendMenuW(menu, MF_STRING, kMenuDebug, L"Debug window");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // required so the menu closes on outside click
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd) DoCommand(cmd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg) { // Explorer restarted; re-add our icon
        UpdateTrayIcon(true);
        return 0;
    }
    switch (msg) {
        case WM_TRAYICON:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
                ShowMenu();
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
                DoCommand(kMenuToggle);
            return 0;
        case WM_COMMAND:
            DoCommand(LOWORD(wp));
            return 0;
        case WM_AUTOCMD:
            if (wp == 1) StartRecording(true);
            else StopRecording(true);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) {
                // Recorder thread may have died (e.g. disk full) — reflect it.
                if (!g_recorder.recording()) {
                    g_recorder.stop();
                    KillTimer(g_hwnd, kTimerId);
                }
                UpdateTrayIcon(false);
            }
            return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    // Hidden debug mode: "--devices" logs the current device pick and exits.
    if (cmdLine && wcsstr(cmdLine, L"--devices")) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DevicePick p = DeviceTracker::Pick();
        LogLine(L"Devices: tracked=%d app=%ls\n  mic=%ls\n  render=%ls",
                p.tracked, p.app.empty() ? L"-" : p.app.c_str(),
                p.micId.c_str(), p.renderId.c_str());
        CoUninitialize();
        return 0;
    }

    // Hidden smoke-test mode: "--record N" records N seconds headless and exits.
    if (cmdLine && wcsstr(cmdLine, L"--record")) {
        int seconds = 5;
        swscanf(cmdLine, L"--record %d", &seconds);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION);
        g_recorder.start(Settings::GetStorageFolder());
        Sleep(seconds * 1000);
        g_recorder.stop();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CallsRecorderSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"CallsRecorder is already running — check the system tray.",
                    L"CallsRecorder", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    LogLine(L"App: started");

    g_inst = hInst;
    g_iconIdle = MakeDotIcon(RGB(110, 130, 150), false);
    g_iconRec = MakeDotIcon(RGB(225, 45, 45), true);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CallsRecorderWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"CallsRecorder", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, hInst, nullptr);
    UpdateTrayIcon(true);

    g_autoEnabled = Settings::GetAutoRecord();
    g_watcher = std::thread(AutoWatcher);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_watcherStop = true;
    if (g_watcher.joinable()) g_watcher.join();
    g_recorder.stop();
    MFShutdown();
    CoUninitialize();
    DestroyIcon(g_iconIdle);
    DestroyIcon(g_iconRec);
    CloseHandle(mutex);
    LogLine(L"App: exited");
    return 0;
}
