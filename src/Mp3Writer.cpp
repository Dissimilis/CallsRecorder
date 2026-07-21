#include "Mp3Writer.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

bool Mp3Writer::open(const std::wstring& path) {
    close();
    framesWritten_ = 0;

    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer_);
    if (FAILED(hr)) {
        LogLine(L"Mp3Writer: MFCreateSinkWriterFromURL failed hr=0x%08X (%ls)", hr, path.c_str());
        return false;
    }

    IMFMediaType* out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kBytesPerSec);
    hr = writer_->AddStream(out, &stream_);
    SafeRelease(out);
    if (FAILED(hr)) {
        LogLine(L"Mp3Writer: AddStream failed hr=0x%08X", hr);
        close();
        return false;
    }

    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
    in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    hr = writer_->SetInputMediaType(stream_, in, nullptr);
    SafeRelease(in);
    if (FAILED(hr)) {
        LogLine(L"Mp3Writer: SetInputMediaType failed hr=0x%08X", hr);
        close();
        return false;
    }

    hr = writer_->BeginWriting();
    if (FAILED(hr)) {
        LogLine(L"Mp3Writer: BeginWriting failed hr=0x%08X", hr);
        close();
        return false;
    }
    LogLine(L"Mp3Writer: opened %ls", path.c_str());
    return true;
}

bool Mp3Writer::write(const int16_t* samples, size_t frames) {
    if (!writer_ || !frames) return writer_ != nullptr;

    DWORD bytes = (DWORD)(frames * kChannels * 2);
    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    buf->Lock(&dst, nullptr, nullptr);
    memcpy(dst, samples, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buf);
    sample->SetSampleTime(framesWritten_ * 10000000LL / kRate);
    sample->SetSampleDuration((long long)frames * 10000000LL / kRate);
    hr = writer_->WriteSample(stream_, sample);
    SafeRelease(sample);
    SafeRelease(buf);

    if (FAILED(hr)) {
        LogLine(L"Mp3Writer: WriteSample failed hr=0x%08X", hr);
        return false;
    }
    framesWritten_ += frames;
    return true;
}

void Mp3Writer::close() {
    if (writer_) {
        writer_->Finalize();
        SafeRelease(writer_);
        LogLine(L"Mp3Writer: closed (%lld frames, %lld s)", framesWritten_, framesWritten_ / kRate);
    }
}
