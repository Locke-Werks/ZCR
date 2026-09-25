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

// Muxes a raw HEVC Annex B elementary stream into fragmented MP4 through
// Mp4Writer, so the container can be checked with ffprobe without a capture
// session or an encoder in the loop.
//
//   mux_test <in.hevc> <out.mp4> <fps> [--hdr] [--size WxH]

#include "mp4mux.h"
#include "win32.h"

#include <cstdio>
#include <cwchar>
#include <string>
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

int Run(int argc, wchar_t** argv)
{
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: mux_test <in.hevc> <out.mp4> <fps> [--hdr] [--size WxH]\n");
        return 2;
    }

    VideoFormat format;
    format.width = 1920;
    format.height = 1080;
    format.fps = static_cast<uint32_t>(std::wcstoul(argv[3], nullptr, 10));
    for (int i = 4; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--hdr") {
            format.hdr = true;
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

    Mp4Writer writer;
    std::wstring error;
    if (!writer.Open(argv[2], format, parameter_sets, error)) {
        std::fwprintf(stderr, L"Open: %ls\n", error.c_str());
        return 1;
    }

    size_t keyframes = 0;
    for (const AccessUnit& unit : units) {
        keyframes += unit.keyframe ? 1 : 0;
        if (!writer.WriteSample(unit.bytes.data(), unit.bytes.size(), unit.keyframe, error)) {
            std::fwprintf(stderr, L"WriteSample: %ls\n", error.c_str());
            return 1;
        }
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
