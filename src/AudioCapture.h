#pragma once
#include "common.h"
#include <cstdint>
#include <vector>

struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

// Linear-interpolation resampler for stereo float frames -> 48 kHz.
// Quality is fine for voice; only used when WASAPI's auto-conversion is unavailable.
struct Resampler {
    double step = 1.0, pos = 0.0;
    bool hasCarry = false;
    float cl = 0, cr = 0;
    void reset(int srcRate) { step = (double)srcRate / 48000.0; pos = 0; hasCarry = false; }
    void process(const float* in, size_t frames, std::vector<float>& out);
};

// One WASAPI shared-mode capture stream (mic, or loopback on a render device).
// Always delivers interleaved 16-bit stereo PCM at 48 kHz via pump().
class AudioCaptureStream {
public:
    // Takes its own reference on dev; caller keeps ownership of the passed pointer.
    bool init(IMMDevice* dev, bool loopback);
    // Appends available audio to out. Returns false when the device died
    // (unplugged/invalidated) and the stream must be re-initialized.
    // Sets *discontinuity when WASAPI reports dropped data (a glitch), so the
    // caller can resynchronize the two streams.
    bool pump(std::vector<int16_t>& out, bool* discontinuity = nullptr);
    void shutdown();
    bool valid() const { return capture_ != nullptr; }
    ~AudioCaptureStream() { shutdown(); }

private:
    void toStereoFloat(const BYTE* data, UINT32 frames, std::vector<float>& out) const;

    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    bool firstPacket_ = true; // WASAPI flags the very first packet as a discontinuity
    bool needConvert_ = false;
    int srcRate_ = 48000, srcCh_ = 2, srcBits_ = 16;
    bool srcFloat_ = false;
    Resampler rs_;
    std::vector<float> tmpF_, tmpR_;
};
