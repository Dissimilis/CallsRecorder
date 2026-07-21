#include "common.h"
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>

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

    std::wstring path = AppDataDir() + L"\\CallsRecorder.log";
    FILE* f = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    fclose(f);
}
