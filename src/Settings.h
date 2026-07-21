#pragma once
#include <string>

namespace Settings {
// Storage folder for recordings. Defaults to the user's Downloads folder.
std::wstring GetStorageFolder();
void SetStorageFolder(const std::wstring& folder);

// Start/stop recording automatically when a call app uses the microphone.
bool GetAutoRecord();
void SetAutoRecord(bool on);

// Record mic to the left channel and call audio to the right instead of mixing.
bool GetSplitChannels();
void SetSplitChannels(bool on);
}
