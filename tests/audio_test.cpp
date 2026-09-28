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

// Captures desktop and microphone audio through AudioSource with no video in
// the loop, and writes each track as ADTS so ffprobe and ffmpeg can check it.
//
//   audio_test [--seconds N] [--mic <id>|--no-mic] [--no-desktop] [--list] [--out DIR]
//
// One N-second segment per source goes to <out>/desktop.aac and <out>/mic.aac.
// Then two more one-second segments follow back to back, each starting where
// the previous one ended (<out>/desktop_1.aac, desktop_2.aac, and mic_1/mic_2),
// the way the recorder rolls over to a new file.

#include "audio.h"
#include "diag_log.h"
#include "win32.h"

#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace zcr {
namespace {

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

void SleepUntil(double qpc_seconds)
{
    for (;;) {
        const double wait = qpc_seconds - QpcSeconds();
        if (wait <= 0.0) {
            return;
        }
        Sleep(static_cast<DWORD>(std::ceil(wait * 1000.0)));
    }
}

/// Writes raw AAC frames as ADTS, taking the header fields from the track's
/// AudioSpecificConfig so a wrong ASC shows up as a file that will not decode.
class AdtsFile {
public:
    bool Open(const std::wstring& path, const AudioTrackConfig& config)
    {
        if (config.specific_config.size() < 2) {
            return false;
        }
        const uint8_t a = config.specific_config[0];
        const uint8_t b = config.specific_config[1];
        object_type_ = a >> 3;
        freq_index_ = static_cast<uint8_t>(((a & 7) << 1) | (b >> 7));
        channel_config_ = (b >> 3) & 0xF;
        path_ = path;
        file_ = _wfopen(path.c_str(), L"wb");
        return file_ != nullptr;
    }

    ~AdtsFile() { Close(); }

    void Close()
    {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void Write(const uint8_t* data, size_t size)
    {
        const size_t full = size + 7;
        uint8_t h[7];
        h[0] = 0xFF;
        h[1] = 0xF1;   // MPEG-4, layer 0, no CRC
        h[2] = static_cast<uint8_t>(((object_type_ - 1) << 6) | (freq_index_ << 2)
                                    | (channel_config_ >> 2));
        h[3] = static_cast<uint8_t>(((channel_config_ & 3) << 6) | (full >> 11));
        h[4] = static_cast<uint8_t>((full >> 3) & 0xFF);
        h[5] = static_cast<uint8_t>(((full & 7) << 5) | 0x1F);   // buffer fullness 0x7FF: VBR
        h[6] = 0xFC;
        std::fwrite(h, 1, sizeof h, file_);
        std::fwrite(data, 1, size, file_);
        ++frames_;
    }

    [[nodiscard]] uint64_t Frames() const { return frames_; }
    [[nodiscard]] const std::wstring& Path() const { return path_; }

private:
    FILE* file_ = nullptr;
    std::wstring path_;
    uint8_t object_type_ = 2;
    uint8_t freq_index_ = 3;
    uint8_t channel_config_ = 2;
    std::atomic<uint64_t> frames_{0};
};

struct Track {
    const wchar_t* stem;
    AudioSource source;
    AudioTrackConfig config;
};

int List()
{
    // The tray calls this from its STA UI thread; the recorder may call it from
    // an MTA worker. Both must see the same devices.
    const std::vector<AudioDevice> sta = EnumerateMicrophones();
    size_t mta_count = 0;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mta_count = EnumerateMicrophones().size();
        CoUninitialize();
    }).join();

    int defaults = 0;
    for (const AudioDevice& d : sta) {
        std::wprintf(L"%ls %ls\n    %ls\n", d.is_default ? L"*" : L" ", d.name.c_str(),
                     d.id.c_str());
        defaults += d.is_default ? 1 : 0;
    }
    std::wprintf(L"%zu microphones (STA), %zu (MTA), %d default\n", sta.size(), mta_count,
                 defaults);
    return sta.size() == mta_count ? 0 : 1;
}

/// Records one segment [origin, origin + seconds) on every track into
/// <out>/<stem><suffix>.aac. Returns false if any file could not be opened.
bool Segment(std::vector<std::unique_ptr<Track>>& tracks, const std::wstring& out,
             const wchar_t* suffix, double origin, double seconds)
{
    std::vector<std::unique_ptr<AdtsFile>> files;
    for (auto& t : tracks) {
        auto file = std::make_unique<AdtsFile>();
        const std::wstring path = JoinPath(out, std::wstring(t->stem) + suffix + L".aac");
        if (!file->Open(path, t->config)) {
            std::fwprintf(stderr, L"cannot create %ls\n", path.c_str());
            return false;
        }
        AdtsFile* f = file.get();
        t->source.Attach([f](const uint8_t* data, size_t size) { f->Write(data, size); },
                         origin);
        files.push_back(std::move(file));
    }

    const double end = origin + seconds;
    SleepUntil(end);
    const double detach_start = QpcSeconds();
    for (auto& t : tracks) {
        t->source.Detach(end);
    }
    const double detach_ms = (QpcSeconds() - detach_start) * 1000.0;

    const double expected = seconds * 48000.0 / 1024.0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        files[i]->Close();
        std::wprintf(L"%-10ls %6llu frames, expected %.2f (%.0f with the padded tail)  %ls\n",
                     (std::wstring(tracks[i]->stem) + suffix).c_str(),
                     static_cast<unsigned long long>(files[i]->Frames()), expected,
                     std::ceil(expected), files[i]->Path().c_str());
    }
    std::wprintf(L"           detach took %.0f ms for %zu source(s)\n", detach_ms, tracks.size());
    return true;
}

int Run(int argc, wchar_t** argv)
{
    double seconds = 5.0;
    bool desktop = true;
    bool mic = true;
    std::wstring mic_id;
    std::wstring out = L"tests\\out";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--list") {
            return List();
        }
        if (arg == L"--seconds" && i + 1 < argc) {
            seconds = std::wcstod(argv[++i], nullptr);
        } else if (arg == L"--mic" && i + 1 < argc) {
            mic_id = argv[++i];
        } else if (arg == L"--no-mic") {
            mic = false;
        } else if (arg == L"--no-desktop") {
            desktop = false;
        } else if (arg == L"--out" && i + 1 < argc) {
            out = argv[++i];
        } else {
            std::fwprintf(stderr,
                          L"usage: audio_test [--seconds N] [--mic <id>|--no-mic] [--no-desktop] "
                          L"[--list] [--out DIR]\n");
            return 2;
        }
    }
    if (seconds <= 0.0 || (!desktop && !mic)) {
        std::fwprintf(stderr, L"nothing to record\n");
        return 2;
    }
    if (!EnsureDirectory(out)) {
        std::fwprintf(stderr, L"cannot create %ls\n", out.c_str());
        return 1;
    }

    std::vector<std::unique_ptr<Track>> tracks;
    auto start = [&](const wchar_t* stem, AudioSourceKind kind, const std::wstring& id) {
        auto t = std::make_unique<Track>();
        t->stem = stem;
        std::wstring error;
        const double t0 = QpcSeconds();
        if (!t->source.Start(kind, id, error)) {
            std::fwprintf(stderr, L"%ls: %ls\n", stem, error.c_str());
            return false;
        }
        t->config = t->source.TrackConfig();
        std::wprintf(L"%-10ls started in %.0f ms: %u Hz %uch %u bps, ASC", stem,
                     (QpcSeconds() - t0) * 1000.0, t->config.sample_rate, t->config.channels,
                     t->config.avg_bitrate);
        for (const uint8_t b : t->config.specific_config) {
            std::wprintf(L" %02X", b);
        }
        std::wprintf(L", \"%ls\"\n", t->config.name.c_str());
        tracks.push_back(std::move(t));
        return true;
    };
    if (desktop && !start(L"desktop", AudioSourceKind::Desktop, {})) {
        return 1;
    }
    if (mic && !start(L"mic", AudioSourceKind::Microphone, mic_id)) {
        return 1;
    }

    const double origin = QpcSeconds() + 0.2;
    if (!Segment(tracks, out, L"", origin, seconds)) {
        return 1;
    }
    // Rollover: each new segment starts exactly where the last one ended, so
    // the first Attach's origin is already in the past by the time it runs.
    const double second = origin + seconds;
    if (!Segment(tracks, out, L"_1", second, 1.0)
        || !Segment(tracks, out, L"_2", second + 1.0, 1.0)) {
        return 1;
    }

    for (auto& t : tracks) {
        t->source.Stop();
    }
    std::wprintf(L"log: %ls\n", log::Path().c_str());
    return 0;
}

} // namespace
} // namespace zcr

int wmain(int argc, wchar_t** argv)
{
    // An STA, like the tray thread that owns AudioSource in the app.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = zcr::Run(argc, argv);
    CoUninitialize();
    return rc;
}
