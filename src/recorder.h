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
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace zcr {

struct RecorderSettings {
    uint32_t fps = 60;               // 30, 60 or 120
    std::wstring monitor_device_path; // MonitorInfo::device_path; empty = primary
    bool cursor = true;
    std::wstring output_dir;         // must exist or be creatable
    uint32_t cq = 24;
    uint32_t max_mbps = 0;           // 0 = derived from pixel rate
};

enum class RecorderState {
    Idle,
    Recording,
    Error,   // last recording failed; `error` says why. Start clears it.
};

struct RecorderStatus {
    RecorderState state = RecorderState::Idle;
    std::wstring file;          // current segment while recording, else last finished file
    std::wstring error;         // set in Error
    uint64_t frames = 0;        // frames written to the current segment
    double seconds = 0.0;       // frames / fps
    uint32_t fps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hdr = false;
    std::wstring monitor;       // friendly name
    uint64_t late_ticks = 0;    // pacer ticks that ran late enough to need catch-up
};

/// Owns one recording at a time: device, capture, converter, encoder, muxer and
/// the two threads (capture/pacer and writer).
///
/// Start and Stop are called from the UI thread only. Status is safe from any
/// thread. When the state changes on its own (a failure mid-recording, or a
/// segment rollover after a display mode change), the recorder posts
/// `notify_msg` to `notify_hwnd` so the tray can recolor; the tray then calls
/// Status().
class Recorder {
public:
    Recorder(HWND notify_hwnd, UINT notify_msg);
    ~Recorder();   // stops and finalizes any recording in progress
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// Blocks until the first frame is captured and the file is open (a few
    /// hundred ms at most). On success `path_or_error` is the output file; on
    /// failure it is the reason, and the state becomes Error.
    bool Start(const RecorderSettings& settings, std::wstring& path_or_error);

    /// Blocks until the encoder is drained and the file finalized. On success
    /// `path_or_error` is the finished file (the last segment if there were
    /// several). Returns false with a reason if nothing was recording.
    bool Stop(std::wstring& path_or_error);

    [[nodiscard]] bool IsRecording() const;
    [[nodiscard]] RecorderStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zcr
