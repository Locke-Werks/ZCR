// ZCR, a zero-copy HDR screen recorder for Windows on NVIDIA hardware.
// Copyright (C) 2026 Locke Werks
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
#include "audio.h"

#include "diag_log.h"
#include "win32.h"

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// Last, so INITGUID defines storage for the property keys in this header only
// and not for every GUID the headers above declare (mfuuid.lib has those).
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

namespace zcr {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kRate = 48000;
constexpr int64_t kFrame = 1024;   // samples per AAC-LC frame

/// Poll period. Loopback in event mode does not signal while nothing plays and
/// is unreliable when it does, so every stream is polled.
constexpr LONGLONG kPollHns = 100000;   // 10 ms

/// WASAPI buffer. Long enough that a poll delayed by a busy machine drains a
/// full buffer instead of losing it.
constexpr REFERENCE_TIME kBufferHns = 2000000;   // 200 ms

/// A packet this close to the write cursor is appended as is. Absorbs timestamp
/// jitter and slow clock drift; beyond it the timeline resyncs to the packet.
constexpr int64_t kJitterSamples = kRate / 50;   // 20 ms

/// How far behind real time the cursor may fall before silence is filled in.
/// Raised automatically for a device whose packets arrive later than this
/// (Bluetooth), or silence would overtake real audio and drop it.
constexpr double kSilenceLagSeconds = 0.100;

/// Recent audio kept while detached, so an Attach whose origin is slightly in
/// the past still gets real samples for it.
constexpr int64_t kHistorySamples = kRate;   // 1 s

/// Detach waits this long for the device to deliver up to the end time before
/// filling the rest with silence.
constexpr double kDetachWaitSeconds = 0.300;

/// Upper bound on Detach as the caller sees it.
constexpr auto kDetachTimeout = std::chrono::milliseconds(1000);

constexpr ULONGLONG kReopenIntervalMs = 1000;

/// A sleep shorter than this is left to the ordinary silence fill. The same as
/// the video pacer's resync threshold (kResyncAfterSeconds in recorder.cpp):
/// below it the pacer catches up with repeated frames and keeps its timeline,
/// above it the pacer skips, and audio has to make the same choice. Real sleeps
/// are minutes, far from the boundary.
constexpr double kSleepSeconds = 1.0;

/// 192 kbps stereo, 96 kbps mono. The in-box encoder accepts only 12000,
/// 16000, 20000 and 24000 bytes per second.
constexpr uint32_t kDesktopBytesPerSecond = 24000;
constexpr uint32_t kMicBytesPerSecond = 12000;

[[nodiscard]] std::wstring HexHr(HRESULT hr)
{
    wchar_t buffer[16];
    std::swprintf(buffer, 16, L"0x%08X", static_cast<unsigned>(hr));
    return buffer;
}

double QpcSeconds()
{
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / freq;
}

/// Timeline positions are absolute sample indices on the QPC clock: sample n
/// sits at QPC time n / 48000. Integer positions keep the cursor exact over a
/// recording of any length, where accumulating doubles would drift.
[[nodiscard]] int64_t ToSample(double seconds)
{
    return std::llround(seconds * kRate);
}

/// Media Foundation is delay-loaded because Windows N ships without it, and a
/// delay-load that cannot find its DLL raises an SEH exception at the first
/// call rather than returning an error. So look for it before calling it.
[[nodiscard]] bool MediaFoundationReady(std::wstring& error)
{
    static const HRESULT state = [] {
        // Never freed: while this System32 copy is loaded the delay-load helper
        // binds to it by name, instead of searching the default directories,
        // which include the exe's own (a planted mfplat.dll in Downloads).
        if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }
        return MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }();
    if (state == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) {
        error = L"audio needs Media Foundation, which this edition of Windows does not include";
        return false;
    }
    if (FAILED(state)) {
        error = L"Media Foundation failed to start: " + HexHr(state);
        return false;
    }
    return true;
}

[[nodiscard]] std::wstring FriendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR
        && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

[[nodiscard]] std::wstring DeviceId(IMMDevice* device)
{
    CoTaskMem id;
    if (FAILED(device->GetId(id.Receive())) || !id.Get()) {
        return {};
    }
    return id.Get();
}

/// AudioSpecificConfig for AAC-LC at 48 kHz: object type 2, frequency index 3,
/// the channel configuration, and three zero bits (960-sample frames off, no
/// core coder, no extension).
[[nodiscard]] std::vector<uint8_t> ComputeAsc(uint32_t channels)
{
    const uint32_t bits = (2u << 11) | (3u << 7) | ((channels & 0xFu) << 3);
    return {static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits & 0xFF)};
}

// ---------------------------------------------------------------------------
// Sample format conversion, for the fallback where WASAPI will not convert to
// 48 kHz for us and the stream runs at the endpoint's mix format instead.
// ---------------------------------------------------------------------------

struct SourceLayout {
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;          // container bits per sample
    uint32_t block_align = 0;
    bool is_float = false;
    DWORD mask = 0;             // speaker positions, 0 when the format has none
};

[[nodiscard]] bool ParseFormat(const WAVEFORMATEX& wf, SourceLayout& out)
{
    out.rate = wf.nSamplesPerSec;
    out.channels = wf.nChannels;
    out.bits = wf.wBitsPerSample;
    out.block_align = wf.nBlockAlign;
    out.mask = 0;
    if (wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        out.is_float = true;
    } else if (wf.wFormatTag == WAVE_FORMAT_PCM) {
        out.is_float = false;
    } else if (wf.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf.cbSize >= 22) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wf);
        out.mask = ext.dwChannelMask;
        if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            out.is_float = true;
        } else if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            out.is_float = false;
        } else {
            return false;
        }
    } else {
        return false;
    }
    if (out.channels == 0 || out.rate == 0 || out.block_align < out.channels * (out.bits / 8)) {
        return false;
    }
    if (out.is_float) {
        return out.bits == 32 || out.bits == 64;
    }
    return out.bits == 8 || out.bits == 16 || out.bits == 24 || out.bits == 32;
}

/// Converts whatever the stream delivers into 48 kHz int16 at the target
/// channel count. The common case, WASAPI already converting to exactly that,
/// is a copy.
class PcmConverter {
public:
    void Init(const SourceLayout& src, uint32_t target_channels)
    {
        src_ = src;
        target_ = target_channels;
        passthrough_ = !src.is_float && src.bits == 16 && src.channels == target_channels
                       && src.rate == kRate && src.block_align == 2 * target_channels;
        step_ = static_cast<double>(src.rate) / kRate;
        pos_ = 0.0;
        last_ = {0.0f, 0.0f};
        BuildMix();
    }

    [[nodiscard]] bool Passthrough() const { return passthrough_; }

    /// `data` null means a silent packet: the same number of frames, all zero,
    /// fed through the resampler so its phase stays continuous.
    void Convert(const BYTE* data, uint32_t frames, std::vector<int16_t>& out)
    {
        if (passthrough_) {
            out.resize(static_cast<size_t>(frames) * target_);
            if (data) {
                std::memcpy(out.data(), data, out.size() * sizeof(int16_t));
            } else {
                std::fill(out.begin(), out.end(), int16_t{0});
            }
            return;
        }

        mixed_.assign(static_cast<size_t>(frames) * target_, 0.0f);
        if (data) {
            for (uint32_t f = 0; f < frames; ++f) {
                const BYTE* frame = data + static_cast<size_t>(f) * src_.block_align;
                float* dst = &mixed_[static_cast<size_t>(f) * target_];
                for (uint32_t c = 0; c < src_.channels; ++c) {
                    const float v = Read(frame + static_cast<size_t>(c) * (src_.bits / 8));
                    for (uint32_t t = 0; t < target_; ++t) {
                        dst[t] += v * mix_[c][t];
                    }
                }
            }
        }

        out.clear();
        if (src_.rate == kRate) {
            out.reserve(mixed_.size());
            for (const float v : mixed_) {
                out.push_back(ToInt16(v));
            }
            return;
        }

        // Linear interpolation. Index -1 is the last frame of the previous
        // packet, so the output is continuous across packet boundaries.
        auto at = [&](int64_t i, uint32_t c) {
            return i < 0 ? last_[c] : mixed_[static_cast<size_t>(i) * target_ + c];
        };
        const double end = static_cast<double>(frames) - 1.0;
        out.reserve(static_cast<size_t>(frames / step_ + 2.0) * target_);
        while (pos_ < end) {
            const double whole = std::floor(pos_);
            const auto i = static_cast<int64_t>(whole);
            const auto frac = static_cast<float>(pos_ - whole);
            for (uint32_t c = 0; c < target_; ++c) {
                out.push_back(ToInt16(at(i, c) * (1.0f - frac) + at(i + 1, c) * frac));
            }
            pos_ += step_;
        }
        pos_ -= static_cast<double>(frames);
        if (frames > 0) {
            for (uint32_t c = 0; c < target_; ++c) {
                last_[c] = mixed_[static_cast<size_t>(frames - 1) * target_ + c];
            }
        }
    }

private:
    [[nodiscard]] static int16_t ToInt16(float v)
    {
        const float clamped = std::clamp(v, -1.0f, 1.0f);
        return static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
    }

    [[nodiscard]] float Read(const BYTE* p) const
    {
        if (src_.is_float) {
            if (src_.bits == 64) {
                double d = 0.0;
                std::memcpy(&d, p, sizeof d);
                return static_cast<float>(d);
            }
            float f = 0.0f;
            std::memcpy(&f, p, sizeof f);
            return f;
        }
        switch (src_.bits) {
        case 8:
            return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        case 16: {
            int16_t v = 0;
            std::memcpy(&v, p, sizeof v);
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            // Into the top three bytes, then an arithmetic shift down to
            // sign-extend.
            const uint32_t u = (static_cast<uint32_t>(p[0]) << 8)
                               | (static_cast<uint32_t>(p[1]) << 16)
                               | (static_cast<uint32_t>(p[2]) << 24);
            return static_cast<float>(static_cast<int32_t>(u) >> 8) / 8388608.0f;
        }
        default: {
            int32_t v = 0;
            std::memcpy(&v, p, sizeof v);
            return static_cast<float>(v) / 2147483648.0f;
        }
        }
    }

    /// Per source channel, its weight into each target channel. Speaker
    /// positions come from the channel mask when there is one; without one,
    /// channel 0 is left and 1 is right, which is what every two-channel
    /// device means.
    void BuildMix()
    {
        constexpr float kHalfPower = 0.7071f;
        mix_.assign(src_.channels, {0.0f, 0.0f});
        if (src_.channels == 1) {
            mix_[0] = {1.0f, 1.0f};   // a mono mic is full level on both sides
        } else {
            DWORD remaining = src_.mask;
            for (uint32_t c = 0; c < src_.channels; ++c) {
                DWORD speaker = 0;
                if (remaining) {
                    speaker = remaining & (~remaining + 1);   // lowest set bit
                    remaining &= ~speaker;
                } else if (c == 0) {
                    speaker = SPEAKER_FRONT_LEFT;
                } else if (c == 1) {
                    speaker = SPEAKER_FRONT_RIGHT;
                }
                switch (speaker) {
                case SPEAKER_FRONT_LEFT:
                    mix_[c] = {1.0f, 0.0f};
                    break;
                case SPEAKER_FRONT_RIGHT:
                    mix_[c] = {0.0f, 1.0f};
                    break;
                case SPEAKER_LOW_FREQUENCY:
                    break;
                case SPEAKER_BACK_LEFT:
                case SPEAKER_SIDE_LEFT:
                case SPEAKER_FRONT_LEFT_OF_CENTER:
                    mix_[c] = {kHalfPower, 0.0f};
                    break;
                case SPEAKER_BACK_RIGHT:
                case SPEAKER_SIDE_RIGHT:
                case SPEAKER_FRONT_RIGHT_OF_CENTER:
                    mix_[c] = {0.0f, kHalfPower};
                    break;
                default:   // centre, back centre, top and unknown positions
                    mix_[c] = {kHalfPower, kHalfPower};
                    break;
                }
            }
        }
        if (target_ == 1) {
            for (auto& w : mix_) {
                w = {src_.channels == 1 ? 1.0f : 0.5f * (w[0] + w[1]), 0.0f};
            }
        }
    }

    SourceLayout src_;
    uint32_t target_ = 2;
    bool passthrough_ = false;
    double step_ = 1.0;
    double pos_ = 0.0;
    std::array<float, 2> last_{};
    std::vector<std::array<float, 2>> mix_;
    std::vector<float> mixed_;
};

// ---------------------------------------------------------------------------
// The in-box Media Foundation AAC encoder.
//
// Priming: it has no delay to compensate. It holds two frames internally (the
// first output arrives after the third input, and the drain returns the last
// two), and decoded sample n lines up with input sample n exactly, so output
// frame k covers timeline samples [k*1024, (k+1)*1024) as the muxer assumes.
// What it cannot avoid is that the first frame of any AAC stream has no frame
// before it to overlap with, so the first ~450 samples (about 10 ms) of each
// file decode as silence ramping up to full level. Feeding a frame of lead-in
// and discarding the first output does not help: the decoder then starts on
// the second frame with the same missing overlap, measured on this encoder.
// Only an edit list could hide it, and the muxer writes none.
// ---------------------------------------------------------------------------

using FrameOut = std::function<void(const uint8_t*, size_t)>;

class AacEncoder {
public:
    bool Open(uint32_t channels, uint32_t bytes_per_second, std::wstring& error)
    {
        Close();
        channels_ = channels;

        MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, MFAudioFormat_PCM};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Audio, MFAudioFormat_AAC};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                               MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT
                                   | MFT_ENUM_FLAG_SORTANDFILTER,
                               &input, &output, &activates, &count);
        if (SUCCEEDED(hr) && count == 0) {
            hr = MF_E_TOPO_CODEC_NOT_FOUND;
        }
        if (SUCCEEDED(hr)) {
            hr = activates[0]->ActivateObject(IID_PPV_ARGS(&mft_));
        }
        for (UINT32 i = 0; i < count; ++i) {
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
        if (FAILED(hr)) {
            error = L"no AAC encoder: " + HexHr(hr);
            Close();
            return false;
        }

        // Output before input: the AAC encoder rejects an input type until it
        // knows what it is encoding to.
        ComPtr<IMFMediaType> out_type;
        hr = MFCreateMediaType(&out_type);
        if (SUCCEEDED(hr)) {
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
            out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_second);
            out_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // raw frames, no ADTS
            out_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            hr = mft_->SetOutputType(0, out_type.Get(), 0);
        }
        if (FAILED(hr)) {
            error = L"AAC encoder refused " + std::to_wstring(channels) + L"ch "
                    + std::to_wstring(bytes_per_second * 8 / 1000) + L" kbps: " + HexHr(hr);
            Close();
            return false;
        }

        ComPtr<IMFMediaType> in_type;
        hr = MFCreateMediaType(&in_type);
        if (SUCCEEDED(hr)) {
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
            in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * channels);
            in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 2 * channels);
            hr = mft_->SetInputType(0, in_type.Get(), 0);
        }
        if (FAILED(hr)) {
            error = L"AAC encoder refused 48 kHz PCM input: " + HexHr(hr);
            Close();
            return false;
        }

        MFT_OUTPUT_STREAM_INFO info{};
        hr = mft_->GetOutputStreamInfo(0, &info);
        if (FAILED(hr) || (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            error = L"AAC encoder output stream unsupported: " + HexHr(hr);
            Close();
            return false;
        }
        out_size_ = std::max<DWORD>(info.cbSize, 8192);

        ReadSpecificConfig();

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void Close()
    {
        mft_.Reset();
        out_sample_.Reset();
        out_buffer_.Reset();
        frames_in_ = 0;
    }

    [[nodiscard]] bool IsOpen() const { return mft_ != nullptr; }
    [[nodiscard]] const std::vector<uint8_t>& SpecificConfig() const { return asc_; }

    /// Encodes one 1024-sample frame and hands every frame the encoder
    /// releases to `out`.
    bool Encode(const int16_t* pcm, const FrameOut& out, std::wstring& error)
    {
        const DWORD bytes = static_cast<DWORD>(kFrame * channels_ * sizeof(int16_t));
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
        if (SUCCEEDED(hr)) {
            BYTE* dst = nullptr;
            hr = buffer->Lock(&dst, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                std::memcpy(dst, pcm, bytes);
                buffer->Unlock();
                hr = buffer->SetCurrentLength(bytes);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSample(&sample);
        }
        if (SUCCEEDED(hr)) {
            hr = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(hr)) {
            // 100 ns units. Nothing downstream reads these; the encoder wants
            // them monotonic.
            sample->SetSampleTime(frames_in_ * kFrame * 10000000 / kRate);
            sample->SetSampleDuration(kFrame * 10000000 / kRate);
            hr = mft_->ProcessInput(0, sample.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                if (!Pull(out, error)) {
                    return false;
                }
                hr = mft_->ProcessInput(0, sample.Get(), 0);
            }
        }
        if (FAILED(hr)) {
            error = L"AAC ProcessInput failed: " + HexHr(hr);
            return false;
        }
        ++frames_in_;
        return Pull(out, error);
    }

    /// End of stream: delivers the frames the encoder still holds.
    bool Drain(const FrameOut& out, std::wstring& error)
    {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        const HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) {
            error = L"AAC drain failed: " + HexHr(hr);
            return false;
        }
        return Pull(out, error);
    }

private:
    bool Pull(const FrameOut& out, std::wstring& error)
    {
        if (!out_sample_) {
            HRESULT hr = MFCreateMemoryBuffer(out_size_, &out_buffer_);
            if (SUCCEEDED(hr)) {
                hr = MFCreateSample(&out_sample_);
            }
            if (SUCCEEDED(hr)) {
                hr = out_sample_->AddBuffer(out_buffer_.Get());
            }
            if (FAILED(hr)) {
                out_sample_.Reset();
                error = L"AAC output buffer: " + HexHr(hr);
                return false;
            }
        }
        for (;;) {
            out_buffer_->SetCurrentLength(0);
            MFT_OUTPUT_DATA_BUFFER db{};
            db.pSample = out_sample_.Get();
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) {
                db.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return true;
            }
            if (FAILED(hr)) {
                error = L"AAC ProcessOutput failed: " + HexHr(hr);
                return false;
            }
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(out_buffer_->Lock(&data, nullptr, &length))) {
                if (length > 0) {
                    out(data, length);
                }
                out_buffer_->Unlock();
            }
        }
    }

    /// MF_MT_USER_DATA on an AAC type is the tail of HEAACWAVEINFO: a 12-byte
    /// payload header, then the AudioSpecificConfig. Checked against what
    /// AAC-LC at 48 kHz must be, because a muxed track with a wrong ASC plays as
    /// noise and nothing downstream would notice.
    void ReadSpecificConfig()
    {
        const std::vector<uint8_t> expected = ComputeAsc(channels_);
        asc_ = expected;
        ComPtr<IMFMediaType> current;
        if (FAILED(mft_->GetOutputCurrentType(0, &current))) {
            return;
        }
        UINT8 blob[64] = {};
        UINT32 size = 0;
        if (FAILED(current->GetBlob(MF_MT_USER_DATA, blob, sizeof blob, &size)) || size < 14) {
            log::Write(L"audio: encoder gave no AudioSpecificConfig, using the computed one");
            return;
        }
        const std::vector<uint8_t> reported(blob + 12, blob + size);
        if (reported != expected) {
            log::Writef(L"audio: encoder AudioSpecificConfig %zu bytes starting %02X %02X is not "
                        L"AAC-LC 48 kHz, using the computed one",
                        reported.size(), reported[0], reported[1]);
        }
    }

    ComPtr<IMFTransform> mft_;
    ComPtr<IMFSample> out_sample_;
    ComPtr<IMFMediaBuffer> out_buffer_;
    DWORD out_size_ = 0;
    uint32_t channels_ = 2;
    LONGLONG frames_in_ = 0;
    std::vector<uint8_t> asc_;
};

// ---------------------------------------------------------------------------
// Default endpoint changes. Called on a thread of the audio service's
// choosing, so it only raises a flag and wakes the capture thread.
// ---------------------------------------------------------------------------
class DefaultDeviceWatcher final : public IMMNotificationClient {
public:
    DefaultDeviceWatcher(EDataFlow flow, HANDLE wake) : flow_(flow), wake_(wake) {}

    std::atomic<bool> changed{false};

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --refs_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == flow_ && role == eConsole) {
            changed = true;
            SetEvent(wake_);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    ~DefaultDeviceWatcher() = default;

    std::atomic<ULONG> refs_{1};
    EDataFlow flow_;
    HANDLE wake_;
};

/// A plain PCM or float WAVEFORMATEX, for asking WASAPI to convert.
[[nodiscard]] WAVEFORMATEX RequestFormat(uint32_t channels, bool is_float)
{
    WAVEFORMATEX wf{};
    wf.wFormatTag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wf.nChannels = static_cast<WORD>(channels);
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = is_float ? 32 : 16;
    wf.nBlockAlign = static_cast<WORD>(channels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
    return wf;
}

/// Failures that mean the device itself is gone or unusable, as opposed to a
/// format it will not take. Trying another format after one of these only
/// replaces a clear error with a confusing one.
[[nodiscard]] bool IsDeviceFailure(HRESULT hr)
{
    return hr == E_ACCESSDENIED || hr == AUDCLNT_E_DEVICE_INVALIDATED
        || hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_SERVICE_NOT_RUNNING
        || hr == AUDCLNT_E_CPUUSAGE_EXCEEDED || hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

} // namespace

// ---------------------------------------------------------------------------
// EnumerateMicrophones
// ---------------------------------------------------------------------------

std::vector<AudioDevice> EnumerateMicrophones()
{
    // The MMDevice enumerator is free-threaded, so an STA caller (the tray)
    // works as well as an MTA one. On a thread with no COM at all this joins
    // the MTA for the duration; RPC_E_CHANGED_MODE means the caller already has
    // an STA, which is fine and must not be uninitialized.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<AudioDevice> out;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> collection;
        UINT count = 0;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator)))
            && SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                                        &collection))
            && SUCCEEDED(collection->GetCount(&count))) {
            std::wstring default_id;
            ComPtr<IMMDevice> default_device;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                              &default_device))) {
                default_id = DeviceId(default_device.Get());
            }
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device;
                if (FAILED(collection->Item(i, &device))) {
                    continue;
                }
                AudioDevice d;
                d.id = DeviceId(device.Get());
                if (d.id.empty()) {
                    continue;
                }
                d.name = FriendlyName(device.Get());
                if (d.name.empty()) {
                    d.name = d.id;
                }
                d.is_default = !default_id.empty() && d.id == default_id;
                out.push_back(std::move(d));
            }
        }
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return out;
}

// ---------------------------------------------------------------------------
// AudioSource
//
// One thread per source does everything: WASAPI, the timeline, the encoder and
// the sink calls. The other threads only post commands (Attach, Detach, Stop)
// under `mu`, which keeps every COM object in the capture thread's MTA and
// every piece of timeline state single-threaded.
// ---------------------------------------------------------------------------

struct AudioSource::Impl {
    AudioSourceKind kind = AudioSourceKind::Desktop;
    std::wstring device_id;
    uint32_t channels = 2;
    uint32_t bytes_per_second = kDesktopBytesPerSecond;
    AudioTrackConfig config;

    std::thread thread;
    Handle wake;   // auto-reset: commands, Stop, default device changes
    std::atomic<bool> stop{false};

    // ----- shared with the caller's threads, under mu ----------------------
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    bool start_done = false;
    bool start_ok = false;
    std::wstring start_error;
    uint64_t generation = 0;    // bumped by every Attach
    bool attached = false;      // as the caller sees it
    bool attach_pending = false;
    uint64_t attach_generation = 0;
    double attach_origin = 0.0;
    bool detach_pending = false;
    uint64_t detach_generation = 0;
    double detach_end = 0.0;
    uint64_t finished_generation = 0;   // last generation whose Detach completed

    // The sink is guarded separately so a Detach that times out can revoke it
    // without waiting for the capture thread, and once revoked, it stays so.
    std::mutex sink_mu;
    AacFrameSink sink;
    uint64_t sink_generation = 0;

    // ----- capture thread only ---------------------------------------------
    ComPtr<IMMDeviceEnumerator> enumerator;
    DefaultDeviceWatcher* watcher = nullptr;   // one reference, released on exit
    bool watcher_registered = false;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    PcmConverter converter;
    std::wstring device_name;
    bool resync = true;           // place the next packet exactly by its timestamp
    bool lost = false;            // the device went away and is being reopened
    ULONGLONG next_reopen = 0;
    uint64_t discontinuities = 0;

    // The timeline: pcm holds samples [pcm_start, pcm_start + frames), and the
    // end of it is the write cursor.
    bool have_cursor = false;
    int64_t pcm_start = 0;
    std::vector<int16_t> pcm;
    double latency = 0.0;   // how late packets arrive, decaying maximum

    // The segment being encoded.
    AacEncoder encoder;
    bool segment = false;
    uint64_t segment_generation = 0;
    int64_t enc_pos = 0;      // next timeline sample to encode

    // The previous pass of the capture loop, on both clocks, to detect a sleep.
    double last_loop_qpc = 0.0;
    ULONGLONG last_loop_unbiased = 0;
    bool detaching = false;
    int64_t detach_sample = 0;
    double detach_deadline = 0.0;
    uint64_t frames_out = 0;
    bool encoder_failed_logged = false;

    std::vector<int16_t> packet;
    std::vector<int16_t> frame;

    [[nodiscard]] const wchar_t* Tag() const
    {
        return kind == AudioSourceKind::Desktop ? L"desktop audio" : L"microphone";
    }

    [[nodiscard]] int64_t Frames() const
    {
        return static_cast<int64_t>(pcm.size() / channels);
    }

    [[nodiscard]] int64_t Cursor() const { return pcm_start + Frames(); }

    // ----- device ----------------------------------------------------------

    [[nodiscard]] std::wstring DescribeOpenError(HRESULT hr, const std::wstring& step) const
    {
        const std::wstring who = device_name.empty() ? std::wstring(Tag()) : device_name;
        switch (hr) {
        case E_ACCESSDENIED:
            if (kind == AudioSourceKind::Microphone) {
                return L"microphone access is off in Settings > Privacy & security > Microphone";
            }
            return L"desktop audio access denied on " + who;
        case AUDCLNT_E_DEVICE_IN_USE:
            return who + L" is in use by another app in exclusive mode";
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return who + L" was disconnected";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:
            return L"the Windows Audio service is not running";
        default:
            return step + L" failed on " + who + L": " + HexHr(hr);
        }
    }

    bool FindDevice(ComPtr<IMMDevice>& device, std::wstring& error)
    {
        HRESULT hr = S_OK;
        if (kind == AudioSourceKind::Desktop) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr)) {
                error = hr == E_NOTFOUND ? std::wstring(L"no audio output device for desktop audio")
                                         : L"default audio output: " + HexHr(hr);
                return false;
            }
        } else if (device_id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            if (FAILED(hr)) {
                error = hr == E_NOTFOUND ? std::wstring(L"no microphone found")
                                         : L"default microphone: " + HexHr(hr);
                return false;
            }
        } else {
            hr = enumerator->GetDevice(device_id.c_str(), &device);
            if (FAILED(hr)) {
                error = L"microphone not found: " + device_id;
                return false;
            }
            DWORD state = 0;
            if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                const std::wstring name = FriendlyName(device.Get());
                error = L"microphone not found: " + (name.empty() ? device_id : name);
                return false;
            }
        }
        return true;
    }

    /// Initialize, trying the formats in order of how little work they leave
    /// us: WASAPI converting to 48 kHz int16 at our channel count, then to
    /// float, then the endpoint's own mix format converted here.
    bool InitializeClient(IMMDevice* device, std::wstring& error)
    {
        const DWORD base = kind == AudioSourceKind::Desktop ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        const DWORD convert =
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        HRESULT hr = S_OK;
        for (const bool is_float : {false, true}) {
            client.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) {
                error = DescribeOpenError(hr, L"Activate");
                return false;
            }
            const WAVEFORMATEX wf = RequestFormat(channels, is_float);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, base | convert, kBufferHns, 0,
                                    &wf, nullptr);
            SourceLayout layout;
            if (SUCCEEDED(hr) && ParseFormat(wf, layout)) {
                converter.Init(layout, channels);
                log::Writef(L"audio: %s on %s, 48 kHz %s %uch converted by WASAPI", Tag(),
                            device_name.c_str(), is_float ? L"float" : L"int16", channels);
                return true;
            }
            if (IsDeviceFailure(hr)) {
                error = DescribeOpenError(hr, L"Initialize");
                return false;
            }
        }
        const HRESULT convert_hr = hr;

        client.Reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) {
            error = DescribeOpenError(hr, L"Activate");
            return false;
        }
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            error = DescribeOpenError(hr, L"GetMixFormat");
            return false;
        }
        SourceLayout layout;
        const bool parsed = ParseFormat(*mix, layout);
        if (parsed) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, base, kBufferHns, 0, mix, nullptr);
        }
        CoTaskMemFree(mix);
        if (!parsed) {
            error = device_name + L" uses a sample format ZCR cannot read";
            return false;
        }
        if (FAILED(hr)) {
            error = DescribeOpenError(hr, L"Initialize");
            return false;
        }
        converter.Init(layout, channels);
        log::Writef(L"audio: %s on %s, WASAPI would not convert (%s), converting %u Hz %u-bit %s "
                    L"%uch here",
                    Tag(), device_name.c_str(), HexHr(convert_hr).c_str(), layout.rate,
                    layout.bits, layout.is_float ? L"float" : L"int", layout.channels);
        return true;
    }

    bool OpenDevice(std::wstring& error)
    {
        CloseDevice();
        ComPtr<IMMDevice> device;
        if (!FindDevice(device, error)) {
            return false;
        }
        device_name = FriendlyName(device.Get());
        if (device_name.empty()) {
            device_name = Tag();
        }
        if (!InitializeClient(device.Get(), error)) {
            client.Reset();
            return false;
        }
        HRESULT hr = client->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }
        if (FAILED(hr)) {
            error = DescribeOpenError(hr, L"Start");
            CloseDevice();
            return false;
        }
        resync = true;
        discontinuities = 0;
        return true;
    }

    void CloseDevice()
    {
        if (client) {
            client->Stop();
        }
        capture.Reset();
        client.Reset();
    }

    void DeviceLost(HRESULT hr)
    {
        if (!lost) {
            log::Writef(L"audio: %s lost %s (%s), filling silence and reopening every second",
                        Tag(), device_name.c_str(), HexHr(hr).c_str());
        }
        lost = true;
        CloseDevice();
        next_reopen = GetTickCount64() + kReopenIntervalMs;
    }

    void TryReopen()
    {
        std::wstring error;
        if (OpenDevice(error)) {
            log::Writef(L"audio: %s reopened on %s", Tag(), device_name.c_str());
            lost = false;
        } else {
            if (!lost) {
                log::Writef(L"audio: %s cannot reopen (%s), retrying every second", Tag(),
                            error.c_str());
            }
            lost = true;
            next_reopen = GetTickCount64() + kReopenIntervalMs;
        }
    }

    // ----- timeline --------------------------------------------------------

    void FillSilenceTo(int64_t target)
    {
        if (!have_cursor) {
            have_cursor = true;
            pcm_start = target;
            pcm.clear();
            return;
        }
        const int64_t gap = target - Cursor();
        if (gap <= 0) {
            return;
        }
        if (gap <= kHistorySamples) {
            pcm.resize(pcm.size() + static_cast<size_t>(gap) * channels, int16_t{0});
            return;
        }
        // A long gap (a device reopen that blocked, a starved thread) is not
        // materialized: the encoder reads anything before pcm_start as silence,
        // so encode what is buffered and move the start past the gap.
        EncodeAvailable();
        pcm.clear();
        pcm_start = target;
    }

    /// The machine slept. The video pacer skips a sleep instead of filling it,
    /// and the recorder re-attaches audio where video resumes, so the timeline
    /// skips it too rather than encoding it as silence, which would push every
    /// later sample of the track back by the length of the sleep.
    void SkipSleep(int64_t target)
    {
        if (!have_cursor || target <= Cursor()) {
            return;
        }
        EncodeAvailable();
        pcm.clear();
        pcm_start = target;
        enc_pos = std::max(enc_pos, target);
    }

    void Place(int64_t at, const int16_t* data, int64_t frames, bool exact)
    {
        if (!have_cursor) {
            have_cursor = true;
            pcm_start = at;
            pcm.clear();
        }
        const int64_t delta = at - Cursor();
        const int64_t tolerance = exact ? 0 : kJitterSamples;
        if (delta > tolerance) {
            FillSilenceTo(at);
        } else if (delta < -tolerance) {
            // Late: this span is already on the timeline (as silence, or as the
            // tail of a packet that ran long). Keep only what is new.
            const int64_t skip = std::min(frames, -delta);
            data += skip * channels;
            frames -= skip;
        }
        if (frames > 0) {
            pcm.insert(pcm.end(), data, data + frames * channels);
        }
    }

    void DrainPackets()
    {
        // Bounded so a device that never reports empty cannot hold the thread.
        for (int guard = 0; guard < 256 && capture; ++guard) {
            UINT32 next = 0;
            HRESULT hr = capture->GetNextPacketSize(&next);
            if (FAILED(hr)) {
                DeviceLost(hr);
                return;
            }
            if (next == 0) {
                return;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                return;
            }
            if (FAILED(hr)) {
                DeviceLost(hr);
                return;
            }
            converter.Convert((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames,
                              packet);
            capture->ReleaseBuffer(frames);

            const double now = QpcSeconds();
            const auto converted = static_cast<int64_t>(packet.size() / channels);
            const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if (discontinuity && ++discontinuities == 1 && !resync) {
                log::Writef(L"audio: %s glitched (data discontinuity), resyncing by timestamp",
                            Tag());
            }

            // The QPC position is in 100 ns units of the QPC clock:
            // 48000 / 10^7 = 3 / 625, kept in integers.
            int64_t at = 0;
            if (qpc_position != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                at = static_cast<int64_t>((qpc_position * 3 + 312) / 625);
                const double late = now - static_cast<double>(at + converted) / kRate;
                latency = std::clamp(std::max(latency, late), 0.0, 1.0);
            } else {
                at = have_cursor ? Cursor() : ToSample(now) - converted;
            }
            Place(at, packet.data(), converted, resync || discontinuity);
            resync = false;
        }
    }

    [[nodiscard]] double SilenceLag() const
    {
        return std::max(kSilenceLagSeconds, latency + 0.050);
    }

    void Trim()
    {
        int64_t keep_from = Cursor() - kHistorySamples;
        if (segment) {
            keep_from = std::min(keep_from, enc_pos);
        }
        const int64_t excess = keep_from - pcm_start;
        // Erasing from the front moves the rest, so do it in batches.
        if (excess >= kRate / 10) {
            const auto n = static_cast<size_t>(std::min(excess, Frames())) * channels;
            pcm.erase(pcm.begin(), pcm.begin() + static_cast<ptrdiff_t>(n));
            pcm_start += static_cast<int64_t>(n / channels);
        }
    }

    // ----- encoding --------------------------------------------------------

    void Deliver(const uint8_t* data, size_t size)
    {
        ++frames_out;
        std::lock_guard lock(sink_mu);
        if (sink && sink_generation == segment_generation) {
            sink(data, size);
        }
    }

    /// One frame at enc_pos. Samples before the buffer or past the cursor are
    /// silence: the former is audio from before a long gap or an origin in the
    /// past, the latter only happens for the padded last frame of a segment.
    void EncodeFrame()
    {
        frame.assign(static_cast<size_t>(kFrame) * channels, int16_t{0});
        const int64_t from = std::max(enc_pos, pcm_start);
        const int64_t to = std::min(enc_pos + kFrame, Cursor());
        if (to > from) {
            std::memcpy(frame.data() + (from - enc_pos) * channels,
                        pcm.data() + (from - pcm_start) * channels,
                        static_cast<size_t>(to - from) * channels * sizeof(int16_t));
        }
        enc_pos += kFrame;

        if (!encoder.IsOpen()) {
            return;
        }
        std::wstring error;
        const FrameOut out = [this](const uint8_t* d, size_t n) { Deliver(d, n); };
        if (!encoder.Encode(frame.data(), out, error)) {
            // Rebuild rather than give up: a fresh encoder costs one frame of
            // fade-in, while a dead one would cut the track short and put every
            // later segment out of step.
            if (!encoder_failed_logged) {
                log::Writef(L"audio: %s encoder failed (%s), restarting it", Tag(),
                            error.c_str());
                encoder_failed_logged = true;
            }
            if (!encoder.Open(channels, bytes_per_second, error)) {
                log::Writef(L"audio: %s encoder restart failed: %s", Tag(), error.c_str());
            }
        }
    }

    void EncodeAvailable()
    {
        if (!segment || !have_cursor) {
            return;
        }
        const int64_t limit = detaching ? detach_sample : INT64_MAX;
        while (enc_pos + kFrame <= Cursor() && enc_pos < limit) {
            EncodeFrame();
        }
    }

    void BeginSegment(uint64_t gen, double origin)
    {
        segment = true;
        segment_generation = gen;
        enc_pos = ToSample(origin);
        detaching = false;
        frames_out = 0;
        encoder_failed_logged = false;
        std::wstring error;
        // A fresh encoder per segment, so each file starts from a clean state.
        if (!encoder.Open(channels, bytes_per_second, error)) {
            log::Writef(L"audio: %s has no encoder for this segment: %s", Tag(), error.c_str());
        }
    }

    void FinishSegment()
    {
        if (!detaching) {
            // Superseded by a new Attach before its Detach arrived (a Detach
            // that timed out): end where the timeline is.
            detach_sample = have_cursor ? Cursor() : enc_pos;
        }
        detaching = true;   // caps EncodeAvailable at detach_sample
        if (have_cursor && Cursor() < detach_sample) {
            FillSilenceTo(detach_sample);
        }
        EncodeAvailable();
        while (enc_pos < detach_sample) {
            EncodeFrame();   // the last one padded with silence past the end
        }
        if (encoder.IsOpen()) {
            std::wstring error;
            const FrameOut out = [this](const uint8_t* d, size_t n) { Deliver(d, n); };
            if (!encoder.Drain(out, error)) {
                log::Writef(L"audio: %s %s", Tag(), error.c_str());
            }
            encoder.Close();
        }
        log::Writef(L"audio: %s segment done, %llu frames", Tag(),
                    static_cast<unsigned long long>(frames_out));
        segment = false;
        detaching = false;
        {
            std::lock_guard lock(mu);
            finished_generation = std::max(finished_generation, segment_generation);
        }
        cv.notify_all();
    }

    void TakeCommands(double now)
    {
        bool attach = false;
        bool detach = false;
        uint64_t attach_gen = 0;
        uint64_t detach_gen = 0;
        double origin = 0.0;
        double end = 0.0;
        {
            std::lock_guard lock(mu);
            attach = std::exchange(attach_pending, false);
            detach = std::exchange(detach_pending, false);
            attach_gen = attach_generation;
            detach_gen = detach_generation;
            origin = attach_origin;
            end = detach_end;
        }

        auto begin_detach = [&] {
            detaching = true;
            detach_sample = ToSample(end);
            detach_deadline = now + kDetachWaitSeconds;
        };

        bool detach_handled = false;
        if (detach && segment && detach_gen == segment_generation) {
            begin_detach();
            detach_handled = true;
        }
        if (attach) {
            if (segment) {
                FinishSegment();
            }
            BeginSegment(attach_gen, origin);
            if (detach && !detach_handled && detach_gen == attach_gen) {
                begin_detach();
                detach_handled = true;
            }
        }
        if (detach && !detach_handled) {
            // Nothing is encoding for that generation any more; just release
            // the waiter.
            {
                std::lock_guard lock(mu);
                finished_generation = std::max(finished_generation, detach_gen);
            }
            cv.notify_all();
        }
    }

    // ----- the capture thread ---------------------------------------------

    bool Setup(std::wstring& error)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = L"audio device enumerator unavailable: " + HexHr(hr);
            return false;
        }
        if (!OpenDevice(error)) {
            return false;
        }

        // A trial encoder, so a missing or broken AAC encoder fails Start
        // rather than every segment, and so the track config carries the ASC
        // the encoder really produces.
        AacEncoder trial;
        if (!trial.Open(channels, bytes_per_second, error)) {
            return false;
        }
        config.specific_config = trial.SpecificConfig();

        // Follow the default endpoint when that is what was asked for. A named
        // microphone stays put: switching it would be a surprise.
        if (kind == AudioSourceKind::Desktop || device_id.empty()) {
            watcher = new DefaultDeviceWatcher(
                kind == AudioSourceKind::Desktop ? eRender : eCapture, wake.Get());
            hr = enumerator->RegisterEndpointNotificationCallback(watcher);
            watcher_registered = SUCCEEDED(hr);
            if (!watcher_registered) {
                log::Writef(L"audio: %s cannot watch default device changes: %s", Tag(),
                            HexHr(hr).c_str());
            }
        }
        return true;
    }

    void Teardown()
    {
        if (watcher_registered) {
            enumerator->UnregisterEndpointNotificationCallback(watcher);
            watcher_registered = false;
        }
        if (watcher) {
            watcher->Release();
            watcher = nullptr;
        }
        CloseDevice();
        encoder.Close();
        segment = false;
        enumerator.Reset();
    }

    void Loop()
    {
        Handle timer(CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS));
        while (!stop) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -kPollHns;
                SetWaitableTimer(timer.Get(), &due, 0, nullptr, nullptr, FALSE);
                const HANDLE handles[] = {wake.Get(), timer.Get()};
                WaitForMultipleObjects(2, handles, FALSE, 100);
            } else {
                WaitForSingleObject(wake.Get(), 10);
            }
            if (stop) {
                break;
            }

            const double now = QpcSeconds();
            ULONGLONG unbiased = 0;
            QueryUnbiasedInterruptTime(&unbiased);
            // QPC counts through sleep and hibernate; the unbiased interrupt
            // time does not. Their difference is exactly how long the machine
            // was down, which no amount of thread starvation can fake.
            if (last_loop_qpc > 0.0) {
                const double slept = (now - last_loop_qpc)
                                     - static_cast<double>(unbiased - last_loop_unbiased) / 1e7;
                if (slept > kSleepSeconds) {
                    log::Writef(L"audio: %s: the machine slept %.1f s, skipping it", Tag(), slept);
                    SkipSleep(ToSample(now - SilenceLag()));
                }
            }
            last_loop_qpc = now;
            last_loop_unbiased = unbiased;

            TakeCommands(now);

            if (watcher && watcher->changed.exchange(false)) {
                log::Writef(L"audio: default %s device changed, switching",
                            kind == AudioSourceKind::Desktop ? L"output" : L"microphone");
                CloseDevice();
                lost = false;
                next_reopen = 0;
            }
            if (!client && GetTickCount64() >= next_reopen) {
                TryReopen();
            }
            if (client) {
                DrainPackets();
            }

            // Loopback delivers nothing while nothing plays, and a lost device
            // delivers nothing at all; either way the track keeps pace with the
            // clock.
            FillSilenceTo(ToSample(QpcSeconds() - SilenceLag()));
            latency = std::max(0.0, latency - 0.0005);

            EncodeAvailable();
            if (segment && detaching
                && (Cursor() >= detach_sample || !client || QpcSeconds() >= detach_deadline)) {
                FinishSegment();
            }
            Trim();
        }
    }

    void Run()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // MMCSS "Audio" keeps a busy machine from starving the poll and
        // overflowing the WASAPI buffer.
        DWORD task_index = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

        std::wstring error;
        bool ok = SUCCEEDED(com);
        if (!ok) {
            error = L"COM initialization failed: " + HexHr(com);
        } else {
            ok = Setup(error);
        }
        {
            std::lock_guard lock(mu);
            start_done = true;
            start_ok = ok;
            start_error = error;
        }
        cv.notify_all();

        if (ok) {
            Loop();
        }
        Teardown();

        {
            std::lock_guard lock(mu);
            finished_generation = generation;
        }
        cv.notify_all();

        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }
};

AudioSource::AudioSource() : impl_(std::make_unique<Impl>()) {}

AudioSource::~AudioSource()
{
    Stop();
}

bool AudioSource::Start(AudioSourceKind kind, const std::wstring& device_id, std::wstring& error)
{
    Impl& d = *impl_;
    Stop();

    if (!MediaFoundationReady(error)) {
        log::Writef(L"audio: start failed: %s", error.c_str());
        return false;
    }

    d.kind = kind;
    d.device_id = kind == AudioSourceKind::Desktop ? std::wstring() : device_id;
    d.channels = kind == AudioSourceKind::Desktop ? 2u : 1u;
    d.bytes_per_second =
        kind == AudioSourceKind::Desktop ? kDesktopBytesPerSecond : kMicBytesPerSecond;
    d.config = AudioTrackConfig{};
    d.config.sample_rate = kRate;
    d.config.channels = d.channels;
    d.config.avg_bitrate = d.bytes_per_second * 8;
    d.config.name = kind == AudioSourceKind::Desktop ? L"Desktop audio" : L"Microphone";

    d.wake.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!d.wake) {
        error = L"CreateEvent failed: " + std::to_wstring(GetLastError());
        return false;
    }
    d.stop = false;
    {
        std::lock_guard lock(d.mu);
        d.start_done = false;
        d.start_ok = false;
        d.start_error.clear();
        d.attached = false;
        d.attach_pending = false;
        d.detach_pending = false;
        d.finished_generation = d.generation;
    }
    d.have_cursor = false;
    d.pcm.clear();
    d.latency = 0.0;
    d.lost = false;
    d.next_reopen = 0;

    d.thread = std::thread([&d] { d.Run(); });

    std::unique_lock lock(d.mu);
    d.cv.wait(lock, [&] { return d.start_done; });
    if (!d.start_ok) {
        error = d.start_error;
        lock.unlock();
        d.thread.join();
        d.wake.Reset();
        log::Writef(L"audio: start failed: %s", error.c_str());
        return false;
    }
    d.running = true;
    return true;
}

void AudioSource::Stop()
{
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.mu);
        d.running = false;
        d.attached = false;
    }
    if (d.thread.joinable()) {
        d.stop = true;
        SetEvent(d.wake.Get());
        d.thread.join();
    }
    {
        std::lock_guard lock(d.sink_mu);
        d.sink = nullptr;
        d.sink_generation = 0;
    }
    d.wake.Reset();
}

AudioTrackConfig AudioSource::TrackConfig() const
{
    return impl_->config;
}

void AudioSource::Attach(AacFrameSink sink, double origin_seconds)
{
    Impl& d = *impl_;
    bool was_attached = false;
    {
        std::lock_guard lock(d.mu);
        if (!d.running) {
            return;
        }
        was_attached = d.attached;
    }
    // Attach over an attached source ends the previous segment where the new
    // one begins, so its sink is finished with before this returns.
    if (was_attached) {
        Detach(origin_seconds);
    }
    {
        std::lock_guard lock(d.mu);
        const uint64_t gen = ++d.generation;
        {
            std::lock_guard sink_lock(d.sink_mu);
            d.sink = std::move(sink);
            d.sink_generation = gen;
        }
        d.attached = true;
        d.attach_pending = true;
        d.attach_generation = gen;
        d.attach_origin = origin_seconds;
    }
    SetEvent(d.wake.Get());
}

void AudioSource::Detach(double end_seconds)
{
    Impl& d = *impl_;
    uint64_t gen = 0;
    {
        std::lock_guard lock(d.mu);
        if (!d.running || !d.attached) {
            return;
        }
        gen = d.generation;
        d.attached = false;
        d.detach_pending = true;
        d.detach_generation = gen;
        d.detach_end = end_seconds;
    }
    SetEvent(d.wake.Get());

    bool finished = false;
    {
        std::unique_lock lock(d.mu);
        finished = d.cv.wait_for(lock, kDetachTimeout,
                                 [&] { return d.finished_generation >= gen; });
    }
    // Revoked either way. On a timeout the capture thread may still be
    // encoding, but anything it produces now goes nowhere, which is the
    // guarantee: the caller is about to close the file the sink writes to.
    {
        std::lock_guard lock(d.sink_mu);
        if (d.sink_generation == gen) {
            d.sink = nullptr;
            d.sink_generation = 0;
        }
    }
    if (!finished) {
        log::Writef(L"audio: %s detach timed out, the segment's audio tail is cut", d.Tag());
    }
}

} // namespace zcr
