#include "common.h"
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <deque>
#include <mutex>

static std::mutex g_logMutex;
static std::deque<std::wstring> g_logRing;
static unsigned long long g_logCount = 0; // total lines ever logged
static const size_t kLogRingMax = 300;

std::wstring AppDataDir() {
    static std::wstring dir = [] {
        std::wstring d;
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
            d = p;
            CoTaskMemFree(p);
        }
        d += L"\\CallsRecorder";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

void LogLine(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamped[1100];
    swprintf(stamped, 1100, L"%02u:%02u:%02u  %ls", st.wHour, st.wMinute, st.wSecond, msg);

    std::lock_guard<std::mutex> lk(g_logMutex); // also serializes the file append
    g_logRing.push_back(stamped);
    g_logCount++;
    while (g_logRing.size() > kLogRingMax) g_logRing.pop_front();

    std::wstring path = AppDataDir() + L"\\CallsRecorder.log";
    FILE* f = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%04u-%02u-%02u %ls\n", st.wYear, st.wMonth, st.wDay, stamped);
    fclose(f);
}

std::vector<std::wstring> LogTail(unsigned long long& cursor) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::vector<std::wstring> out;
    unsigned long long first = g_logCount - g_logRing.size(); // index of ring[0]
    if (cursor < first) cursor = first;
    for (size_t i = (size_t)(cursor - first); i < g_logRing.size(); i++)
        out.push_back(g_logRing[i]);
    cursor = g_logCount;
    return out;
}
