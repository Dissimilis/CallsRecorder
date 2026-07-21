#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

// Appends a timestamped line to %APPDATA%\CallsRecorder\CallsRecorder.log
// and to an in-memory ring buffer (for the debug window).
void LogLine(const wchar_t* fmt, ...);

// Returns log lines appended since *cursor and advances it. Pass 0 to get
// the whole retained tail. Thread-safe.
std::vector<std::wstring> LogTail(unsigned long long& cursor);

// %APPDATA%\CallsRecorder (created on first use)
std::wstring AppDataDir();

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}
