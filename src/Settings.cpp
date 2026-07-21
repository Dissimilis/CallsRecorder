#include "common.h"
#include "Settings.h"
#include <shlobj.h>

static std::wstring IniPath() {
    return AppDataDir() + L"\\settings.ini";
}

static std::wstring DefaultFolder() {
    std::wstring d;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) {
        d = p;
        CoTaskMemFree(p);
    } else {
        d = L"C:\\CallRecordings";
    }
    return d;
}

std::wstring Settings::GetStorageFolder() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"main", L"folder", L"", buf, MAX_PATH * 2, IniPath().c_str());
    if (buf[0]) return buf;
    return DefaultFolder();
}

void Settings::SetStorageFolder(const std::wstring& folder) {
    std::wstring ini = IniPath();
    // Pre-create as UTF-16 so WritePrivateProfileStringW keeps Unicode
    // (a fresh INI would otherwise be ANSI and mangle non-ASCII paths).
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t bom[] = L"\xFEFF[main]\r\n";
            DWORD written = 0;
            WriteFile(h, bom, (DWORD)(wcslen(bom) * 2), &written, nullptr);
            CloseHandle(h);
        }
    }
    WritePrivateProfileStringW(L"main", L"folder", folder.c_str(), ini.c_str());
}
