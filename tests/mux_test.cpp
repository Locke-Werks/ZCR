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

// Muxes a raw HEVC Annex B elementary stream, plus any number of ADTS AAC
// streams, into fragmented MP4 through Mp4Writer, so the container can be
// checked with ffprobe without a capture session or an encoder in the loop.
//
//   mux_test <in.hevc> <out.mp4> <fps> [--hdr] [--size WxH]
//            [--aac <in.aac> [--aac-name NAME]]...
//            [--order interleave|video-first|audio-first] [--threaded]
//
// Each --aac adds one audio track; --aac-name names the track given just
// before it. The ADTS headers are stripped and the AudioSpecificConfig is
// built from the first one.
//
// --order decides how audio reaches the writer. interleave (the default)
// feeds every audio frame that starts at or before a video sample ahead of
// that sample, the way capture delivers them. video-first writes all video
// and then all audio, and audio-first the reverse; both pile up enough audio
// to exercise the audio-only fragment and the flush on Close. --threaded
// feeds each audio track from its own thread, unpaced, against the video on
// the main thread, which exercises the writer's locking.

#include "mp4mux.h"
#include "win32.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace zcr {
namespace {

struct AccessUnit {
    std::vector<uint8_t> bytes; // Annex B, four-byte start codes
    bool keyframe = false;
    bool has_slice = false;
};

[[nodiscard]] bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart > (1ll << 31)) {
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!out.empty()
        && !ReadFile(file.Get(), out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
        return false;
    }
    out.resize(read);
    return true;
}

struct NalRef {
    const uint8_t* data;
    size_t size;
};

[[nodiscard]] std::vector<NalRef> SplitNals(const std::vector<uint8_t>& s)
{
    std::vector<NalRef> nals;
    const size_t n = s.size();
    size_t start = SIZE_MAX;
    auto close = [&](size_t end) {
        while (end > start && s[end - 1] == 0) {
            --end;
        }
        if (end - start >= 2) {
            nals.push_back({s.data() + start, end - start});
        }
    };
    size_t i = 0;
    while (i + 2 < n) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            if (start != SIZE_MAX) {
                close(i);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) {
        close(n);
    }
    return nals;
}

/// H.265 7.4.2.4.4: these NAL types may only precede the first slice of an
/// access unit, so meeting one after a slice means a new unit has begun.
[[nodiscard]] bool StartsNewUnit(uint8_t type)
{
    return type == 32 || type == 33 || type == 34 || type == 35 || type == 39
        || (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
}

[[nodiscard]] std::vector<AccessUnit> SplitAccessUnits(const std::vector<NalRef>& nals)
{
    std::vector<AccessUnit> units;
    AccessUnit current;
    auto finish = [&] {
        if (current.has_slice) {
            units.push_back(std::move(current));
        }
        current = AccessUnit{};
    };

    for (const NalRef& nal : nals) {
        const uint8_t type = (nal.data[0] >> 1) & 0x3F;
        const bool vcl = type < 32;
        const bool first_slice = vcl && nal.size > 2 && (nal.data[2] & 0x80) != 0;

        if (current.has_slice && ((vcl && first_slice) || (!vcl && StartsNewUnit(type)))) {
            finish();
        }

        static const uint8_t start_code[4] = {0, 0, 0, 1};
        current.bytes.insert(current.bytes.end(), start_code, start_code + 4);
        current.bytes.insert(current.bytes.end(), nal.data, nal.data + nal.size);
        if (vcl) {
            current.has_slice = true;
            if (type >= 16 && type <= 21) {
                current.keyframe = true;
            }
        }
    }
    finish();
    return units;
}

struct AacStream {
    std::wstring path;
    AudioTrackConfig config;
    std::vector<std::vector<uint8_t>> frames; // raw, ADTS header removed
};

/// ISO/IEC 13818-7 ADTS. Every frame must repeat the first one's format,
/// since the track has a single sample entry.
[[nodiscard]] bool ParseAdts(const std::vector<uint8_t>& s, AacStream& out, std::wstring& error)
{
    static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000,  7350};
    uint32_t first_format = UINT32_MAX;
    size_t payload_bytes = 0;
    size_t i = 0;
    while (i + 7 <= s.size()) {
        const uint8_t* h = s.data() + i;
        if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) {
            error = L"no ADTS syncword at byte " + std::to_wstring(i);
            return false;
        }
        const bool crc = (h[1] & 0x01) == 0;
        const uint32_t profile = h[2] >> 6;
        const uint32_t rate_index = (h[2] >> 2) & 0x0F;
        const uint32_t channels = ((h[2] & 0x01) << 2) | (h[3] >> 6);
        const size_t length = (static_cast<size_t>(h[3] & 0x03) << 11)
            | (static_cast<size_t>(h[4]) << 3) | (h[5] >> 5);
        const uint32_t blocks = h[6] & 0x03;
        const size_t header = crc ? 9 : 7;
        if (rate_index >= 13 || channels == 0 || blocks != 0 || length <= header
            || i + length > s.size()) {
            error = L"unsupported or truncated ADTS frame at byte " + std::to_wstring(i);
            return false;
        }
        const uint32_t frame_format = (profile << 8) | (rate_index << 4) | channels;
        if (first_format == UINT32_MAX) {
            first_format = frame_format;
            const uint32_t object_type = profile + 1;
            const uint32_t asc = (object_type << 11) | (rate_index << 7) | (channels << 3);
            out.config.specific_config = {static_cast<uint8_t>(asc >> 8),
                                          static_cast<uint8_t>(asc)};
            out.config.sample_rate = rates[rate_index];
            out.config.channels = channels;
        } else if (frame_format != first_format) {
            error = L"ADTS format changes at byte " + std::to_wstring(i);
            return false;
        }
        out.frames.emplace_back(h + header, h + length);
        payload_bytes += length - header;
        i += length;
    }
    if (out.frames.empty()) {
        error = L"no ADTS frames";
        return false;
    }
    const double seconds = static_cast<double>(out.frames.size()) * 1024.0
        / static_cast<double>(out.config.sample_rate);
    out.config.avg_bitrate = static_cast<uint32_t>(static_cast<double>(payload_bytes) * 8.0 / seconds);
    return true;
}

int Run(int argc, wchar_t** argv)
{
    if (argc < 4) {
        std::fwprintf(stderr,
                      L"usage: mux_test <in.hevc> <out.mp4> <fps> [--hdr] [--size WxH]\n"
                      L"                [--aac <in.aac> [--aac-name NAME]]...\n"
                      L"                [--order interleave|video-first|audio-first] [--threaded]\n");
        return 2;
    }

    VideoFormat format;
    format.width = 1920;
    format.height = 1080;
    format.fps = static_cast<uint32_t>(std::wcstoul(argv[3], nullptr, 10));
    std::vector<AacStream> aac;
    std::wstring order = L"interleave";
    bool threaded = false;
    for (int i = 4; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--hdr") {
            format.hdr = true;
        } else if (arg == L"--aac" && i + 1 < argc) {
            AacStream stream;
            stream.path = argv[++i];
            stream.config.name = L"Audio " + std::to_wstring(aac.size() + 1);
            aac.push_back(std::move(stream));
        } else if (arg == L"--aac-name" && i + 1 < argc) {
            if (aac.empty()) {
                std::fwprintf(stderr, L"--aac-name must follow --aac\n");
                return 2;
            }
            aac.back().config.name = argv[++i];
        } else if (arg == L"--order" && i + 1 < argc) {
            order = argv[++i];
            if (order != L"interleave" && order != L"video-first" && order != L"audio-first") {
                std::fwprintf(stderr, L"bad --order\n");
                return 2;
            }
        } else if (arg == L"--threaded") {
            threaded = true;
        } else if (arg == L"--size" && i + 1 < argc) {
            unsigned w = 0;
            unsigned h = 0;
            if (std::swscanf(argv[++i], L"%ux%u", &w, &h) != 2) {
                std::fwprintf(stderr, L"bad --size\n");
                return 2;
            }
            format.width = w;
            format.height = h;
        } else {
            std::fwprintf(stderr, L"unknown argument: %ls\n", arg.c_str());
            return 2;
        }
    }

    std::vector<uint8_t> stream;
    if (!ReadWholeFile(argv[1], stream)) {
        std::fwprintf(stderr, L"cannot read %ls\n", argv[1]);
        return 1;
    }

    const std::vector<NalRef> nals = SplitNals(stream);
    const std::vector<AccessUnit> units = SplitAccessUnits(nals);
    if (units.empty()) {
        std::fwprintf(stderr, L"no access units in %ls\n", argv[1]);
        return 1;
    }

    std::vector<uint8_t> parameter_sets;
    for (const NalRef& nal : SplitNals(units.front().bytes)) {
        const uint8_t type = (nal.data[0] >> 1) & 0x3F;
        if (type >= 32 && type <= 34) {
            static const uint8_t start_code[4] = {0, 0, 0, 1};
            parameter_sets.insert(parameter_sets.end(), start_code, start_code + 4);
            parameter_sets.insert(parameter_sets.end(), nal.data, nal.data + nal.size);
        }
    }

    std::vector<AudioTrackConfig> audio;
    for (AacStream& in : aac) {
        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(in.path, bytes)) {
            std::fwprintf(stderr, L"cannot read %ls\n", in.path.c_str());
            return 1;
        }
        std::wstring why;
        if (!ParseAdts(bytes, in, why)) {
            std::fwprintf(stderr, L"%ls: %ls\n", in.path.c_str(), why.c_str());
            return 1;
        }
        std::wprintf(L"%ls: %zu frames, %u Hz, %u ch, %u bps, \"%ls\"\n", in.path.c_str(),
                     in.frames.size(), in.config.sample_rate, in.config.channels,
                     in.config.avg_bitrate, in.config.name.c_str());
        audio.push_back(in.config);
    }

    Mp4Writer writer;
    std::wstring error;
    if (!writer.Open(argv[2], format, parameter_sets, audio, error)) {
        std::fwprintf(stderr, L"Open: %ls\n", error.c_str());
        return 1;
    }

    // Next frame to write, per audio track.
    std::vector<size_t> next(aac.size(), 0);
    auto feed_audio = [&](size_t t, size_t until, std::wstring& why) {
        for (; next[t] < until && next[t] < aac[t].frames.size(); ++next[t]) {
            const std::vector<uint8_t>& frame = aac[t].frames[next[t]];
            if (!writer.WriteAudioFrame(t, frame.data(), frame.size(), why)) {
                return false;
            }
        }
        return true;
    };
    auto feed_all_audio = [&] {
        for (size_t t = 0; t < aac.size(); ++t) {
            if (!feed_audio(t, SIZE_MAX, error)) {
                std::fwprintf(stderr, L"WriteAudioFrame: %ls\n", error.c_str());
                return false;
            }
        }
        return true;
    };

    std::vector<std::thread> threads;
    std::vector<std::wstring> thread_errors(aac.size());
    std::vector<int> thread_ok(aac.size(), 1);
    if (threaded) {
        for (size_t t = 0; t < aac.size(); ++t) {
            threads.emplace_back([&, t] {
                thread_ok[t] = feed_audio(t, SIZE_MAX, thread_errors[t]) ? 1 : 0;
            });
        }
    } else if (order == L"audio-first" && !feed_all_audio()) {
        return 1;
    }

    size_t keyframes = 0;
    for (size_t n = 0; n < units.size(); ++n) {
        const AccessUnit& unit = units[n];
        if (!threaded && order == L"interleave") {
            for (size_t t = 0; t < aac.size(); ++t) {
                // Frame k starts at k * 1024 / rate, sample n at n / fps; feed
                // every frame whose start is not after this sample's.
                const uint64_t rate = aac[t].config.sample_rate;
                const size_t until = static_cast<size_t>(n * rate / (1024ull * format.fps)) + 1;
                if (!feed_audio(t, until, error)) {
                    std::fwprintf(stderr, L"WriteAudioFrame: %ls\n", error.c_str());
                    return 1;
                }
            }
        }
        keyframes += unit.keyframe ? 1 : 0;
        if (!writer.WriteSample(unit.bytes.data(), unit.bytes.size(), unit.keyframe, error)) {
            std::fwprintf(stderr, L"WriteSample: %ls\n", error.c_str());
            return 1;
        }
    }

    for (std::thread& thread : threads) {
        thread.join();
    }
    for (size_t t = 0; t < aac.size(); ++t) {
        if (!thread_ok[t]) {
            std::fwprintf(stderr, L"WriteAudioFrame (track %zu): %ls\n", t,
                          thread_errors[t].c_str());
            return 1;
        }
    }
    if (!threaded && !feed_all_audio()) {
        return 1;
    }

    if (!writer.Close(error)) {
        std::fwprintf(stderr, L"Close: %ls\n", error.c_str());
        return 1;
    }
    // A second Close must be a harmless no-op.
    if (!writer.Close(error)) {
        std::fwprintf(stderr, L"second Close: %ls\n", error.c_str());
        return 1;
    }
    // Audio threads can outlive the file by a frame; that must be refused.
    const uint8_t late[1] = {0};
    if (!aac.empty() && writer.WriteAudioFrame(0, late, sizeof(late), error)) {
        std::fwprintf(stderr, L"WriteAudioFrame after Close succeeded\n");
        return 1;
    }

    std::wprintf(L"%zu NAL units, %llu samples, %zu keyframes, %llu bytes\n", nals.size(),
                 static_cast<unsigned long long>(writer.SampleCount()), keyframes,
                 static_cast<unsigned long long>(writer.BytesWritten()));
    return 0;
}

} // namespace
} // namespace zcr

int wmain(int argc, wchar_t** argv)
{
    return zcr::Run(argc, argv);
}
