#include "AudioCapture.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cmath>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubPcm   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    if (!frames) return;
    std::vector<float> w;
    w.reserve((frames + 1) * 2);
    if (hasCarry) { w.push_back(cl); w.push_back(cr); }
    w.insert(w.end(), in, in + frames * 2);
    size_t wf = w.size() / 2;
    if (wf < 2) { cl = w[0]; cr = w[1]; hasCarry = true; return; }
    while (pos <= (double)(wf - 1) - 1e-9) {
        size_t i = (size_t)pos;
        double f = pos - i;
        if (i + 1 >= wf) break;
        out.push_back((float)(w[i * 2] * (1 - f) + w[(i + 1) * 2] * f));
        out.push_back((float)(w[i * 2 + 1] * (1 - f) + w[(i + 1) * 2 + 1] * f));
        pos += step;
    }
    cl = w[(wf - 1) * 2];
    cr = w[(wf - 1) * 2 + 1];
    hasCarry = true;
    pos -= (double)(wf - 1);
    if (pos < 0) pos = 0;
}

bool AudioCaptureStream::init(IMMDevice* dev, bool loopback) {
    shutdown();
    if (!dev) return false;

    DWORD loopFlag = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    const REFERENCE_TIME kBufDuration = 2000000; // 200 ms

    WAVEFORMATEX want = {};
    want.wFormatTag = WAVE_FORMAT_PCM;
    want.nChannels = 2;
    want.nSamplesPerSec = 48000;
    want.wBitsPerSample = 16;
    want.nBlockAlign = 4;
    want.nAvgBytesPerSec = 192000;

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
    if (FAILED(hr)) {
        LogLine(L"AudioCapture: Activate(IAudioClient) failed hr=0x%08X", hr);
        return false;
    }

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             loopFlag | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             kBufDuration, 0, &want, nullptr);
    if (SUCCEEDED(hr)) {
        needConvert_ = false;
    } else {
        // Fall back to the device mix format and convert ourselves.
        SafeRelease(client_);
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
        if (FAILED(hr)) return false;

        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            LogLine(L"AudioCapture: GetMixFormat failed hr=0x%08X", hr);
            shutdown();
            return false;
        }
        srcRate_ = (int)mix->nSamplesPerSec;
        srcCh_ = mix->nChannels;
        srcBits_ = mix->wBitsPerSample;
        srcFloat_ = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = (WAVEFORMATEXTENSIBLE*)mix;
            srcFloat_ = IsEqualGUID(ext->SubFormat, kSubFloat) != 0;
        }
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, loopFlag, kBufDuration, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            LogLine(L"AudioCapture: Initialize(mix) failed hr=0x%08X", hr);
            shutdown();
            return false;
        }
        needConvert_ = true;
        rs_.reset(srcRate_);
        LogLine(L"AudioCapture: manual convert path (%d Hz, %d ch, %d bit, float=%d)",
                srcRate_, srcCh_, srcBits_, srcFloat_);
        bool supported = (srcFloat_ && srcBits_ == 32) ||
                         (!srcFloat_ && (srcBits_ == 16 || srcBits_ == 24 || srcBits_ == 32));
        if (!supported) {
            LogLine(L"AudioCapture: unsupported sample format (%d bit, float=%d) — refusing to record silence",
                    srcBits_, srcFloat_);
            shutdown();
            return false;
        }
    }

    hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_);
    if (FAILED(hr)) {
        LogLine(L"AudioCapture: GetService failed hr=0x%08X", hr);
        shutdown();
        return false;
    }
    hr = client_->Start();
    if (FAILED(hr)) {
        LogLine(L"AudioCapture: Start failed hr=0x%08X", hr);
        shutdown();
        return false;
    }
    return true;
}

void AudioCaptureStream::toStereoFloat(const BYTE* data, UINT32 frames, std::vector<float>& out) const {
    out.reserve(out.size() + frames * 2);
    int ch = srcCh_ > 0 ? srcCh_ : 1;
    for (UINT32 i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (srcFloat_ && srcBits_ == 32) {
            const float* s = (const float*)data + (size_t)i * ch;
            l = s[0];
            r = ch > 1 ? s[1] : s[0];
        } else if (srcBits_ == 16) {
            const int16_t* s = (const int16_t*)data + (size_t)i * ch;
            l = s[0] / 32768.0f;
            r = (ch > 1 ? s[1] : s[0]) / 32768.0f;
        } else if (srcBits_ == 32) {
            const int32_t* s = (const int32_t*)data + (size_t)i * ch;
            l = (float)(s[0] / 2147483648.0);
            r = (float)((ch > 1 ? s[1] : s[0]) / 2147483648.0);
        } else if (srcBits_ == 24) { // packed 24-bit PCM
            auto s24 = [](const BYTE* p) {
                int v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v |= ~0xFFFFFF;
                return (float)(v / 8388608.0);
            };
            const BYTE* p = data + (size_t)i * ch * 3;
            l = s24(p);
            r = ch > 1 ? s24(p + 3) : l;
        }
        out.push_back(l);
        out.push_back(r);
    }
}

static void AppendInt16(const std::vector<float>& in, std::vector<int16_t>& out) {
    out.reserve(out.size() + in.size());
    for (float v : in) {
        int s = (int)lrintf(v * 32767.0f);
        out.push_back((int16_t)std::clamp(s, -32768, 32767));
    }
}

bool AudioCaptureStream::pump(std::vector<int16_t>& out, bool* discontinuity) {
    if (!capture_) return false;
    for (;;) {
        UINT32 pkt = 0;
        HRESULT hr = capture_->GetNextPacketSize(&pkt);
        if (FAILED(hr)) return false;
        if (!pkt) return true;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) return false;

        if (discontinuity && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !firstPacket_)
            *discontinuity = true;
        firstPacket_ = false;

        if (frames) {
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!needConvert_) {
                if (silent) {
                    out.insert(out.end(), (size_t)frames * 2, 0);
                } else {
                    const int16_t* s = (const int16_t*)data;
                    out.insert(out.end(), s, s + (size_t)frames * 2);
                }
            } else {
                tmpF_.clear();
                if (silent) {
                    tmpF_.resize((size_t)frames * 2, 0.0f);
                } else {
                    toStereoFloat(data, frames, tmpF_);
                }
                if (srcRate_ == 48000) {
                    AppendInt16(tmpF_, out);
                } else {
                    tmpR_.clear();
                    rs_.process(tmpF_.data(), tmpF_.size() / 2, tmpR_);
                    AppendInt16(tmpR_, out);
                }
            }
        }
        capture_->ReleaseBuffer(frames);
    }
}

void AudioCaptureStream::shutdown() {
    if (client_) client_->Stop();
    SafeRelease(capture_);
    SafeRelease(client_);
    firstPacket_ = true;
    needConvert_ = false;
    srcRate_ = 48000;
    srcCh_ = 2;
    srcBits_ = 16;
    srcFloat_ = false;
}
