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

#include "recorder.h"

#include <cstdint>
#include <string>

namespace zcr {

/// %LOCALAPPDATA%\ZCR\zcr.json.
///
/// Only the tray instance reads or writes it. Command-line changes travel over
/// the pipe and are applied by the tray, so there is a single writer and no
/// read-modify-write race between processes.
struct Settings {
    uint32_t fps = 60;          // 30, 60 or 120
    std::wstring monitor;       // MonitorInfo::device_path; empty = primary
    bool cursor = true;
    std::wstring output_dir;    // as written in the file; empty = the default
    uint32_t cq = 24;           // NVENC constant quality, 1..51
    uint32_t max_mbps = 0;      // 0 = derived from pixel rate
    uint32_t chroma = 444;      // 444 or 420
    // Audio is opt-in. Off by default, so a recording is silent until asked.
    bool desktop_audio = false;
    bool mic = false;
    std::wstring mic_device;    // AudioDevice::id; empty = the default microphone

    /// Missing file: defaults, `detail` empty. Unreadable or unparseable file:
    /// defaults, and `detail` says why so the caller can log it. The file is
    /// never rewritten here; only an explicit change from the user does that.
    [[nodiscard]] static Settings Load(std::wstring& detail);

    /// Writes atomically (temp file and rename). If the file on disk does not
    /// parse, it is kept as zcr.json.bad before being replaced, because it is the
    /// only copy of whatever the person typed into it.
    bool Save(std::wstring& error) const;

    /// output_dir with environment variables expanded, or FOLDERID_Videos\ZCR.
    [[nodiscard]] std::wstring ResolvedOutputDir() const;

    [[nodiscard]] RecorderSettings ToRecorder() const;

    [[nodiscard]] static std::wstring FilePath();
    [[nodiscard]] static bool IsSupportedFps(uint32_t fps);
    [[nodiscard]] static bool IsSupportedChroma(uint32_t chroma);
};

} // namespace zcr
