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

// Pre-create the INI as UTF-16 so WritePrivateProfileStringW keeps Unicode
// (a fresh INI would otherwise be ANSI and mangle non-ASCII folder paths).
// Must run before EVERY write, since any write can be the one creating the file.
static void EnsureUnicodeIni(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const wchar_t bom[] = L"\xFEFF[main]\r\n";
        DWORD written = 0;
        WriteFile(h, bom, (DWORD)(wcslen(bom) * 2), &written, nullptr);
        CloseHandle(h);
    }
}

void Settings::SetStorageFolder(const std::wstring& folder) {
    std::wstring ini = IniPath();
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(L"main", L"folder", folder.c_str(), ini.c_str());
}

static bool GetBool(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(L"main", key, def ? 1 : 0, IniPath().c_str()) != 0;
}

static void SetBool(const wchar_t* key, bool on) {
    std::wstring ini = IniPath();
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(L"main", key, on ? L"1" : L"0", ini.c_str());
}

bool Settings::GetAutoRecord() { return GetBool(L"autoRecord", false); }
void Settings::SetAutoRecord(bool on) { SetBool(L"autoRecord", on); }

bool Settings::GetSplitChannels() { return GetBool(L"splitChannels", false); }
void Settings::SetSplitChannels(bool on) { SetBool(L"splitChannels", on); }
