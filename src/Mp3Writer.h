#pragma once
#include "common.h"
#include <cstdint>

struct IMFSinkWriter;

// Progressive MP3 file writer (Media Foundation sink writer).
// Input: interleaved 16-bit stereo PCM at 48 kHz. Output: 128 kbps CBR MP3.
// The file on disk is a valid, playable MP3 at all times while writing.
class Mp3Writer {
public:
    bool open(const std::wstring& path);
    bool write(const int16_t* samples, size_t frames); // frames = stereo sample pairs
    void close();
    ~Mp3Writer() { close(); }

    static const int kRate = 48000;
    static const int kChannels = 2;
    static const int kBytesPerSec = 16000; // 128 kbps

private:
    IMFSinkWriter* writer_ = nullptr;
    DWORD stream_ = 0;
    long long framesWritten_ = 0;
};
