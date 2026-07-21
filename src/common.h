#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// Appends a timestamped line to %APPDATA%\CallsRecorder\CallsRecorder.log
void LogLine(const wchar_t* fmt, ...);

// %APPDATA%\CallsRecorder (created on first use)
std::wstring AppDataDir();

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}
