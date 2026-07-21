#pragma once
#include <string>

namespace Settings {
// Storage folder for recordings. Defaults to Documents\CallRecordings.
std::wstring GetStorageFolder();
void SetStorageFolder(const std::wstring& folder);
}
