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
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mp4mux.h"

namespace zcr {

/// A capture endpoint, as the tray menu and --list-mics show it.
struct AudioDevice {
    std::wstring id;     // IMMDevice::GetId, what settings store
    std::wstring name;   // PKEY_Device_FriendlyName
    bool is_default = false;   // the eConsole default capture endpoint
};

/// Active capture endpoints, in the order IMMDeviceEnumerator returns them.
/// Callable from any thread that has COM initialized, STA or MTA. Empty on
/// failure.
[[nodiscard]] std::vector<AudioDevice> EnumerateMicrophones();

enum class AudioSourceKind {
    Desktop,      // loopback of the default render endpoint, 48 kHz stereo
    Microphone,   // a capture endpoint, 48 kHz mono
};

/// Receives one raw AAC frame (1024 samples). Called on the source's own thread.
using AacFrameSink = std::function<void(const uint8_t* data, size_t size)>;

/// One audio source: a WASAPI shared-mode stream, a capture thread, and an AAC
/// encoder, producing a gapless 48 kHz timeline locked to QueryPerformanceCounter.
///
/// Times are QPC seconds: QueryPerformanceCounter / QueryPerformanceFrequency,
/// the same clock the video pacer uses, so timeline sample n of a target sits at
/// origin + n / 48000 on both.
///
/// Silence is real samples: a loopback stream delivers nothing while nothing
/// plays, and a gap is filled with zeros so the track stays aligned. A device
/// that disappears mid-recording (a USB mic unplugged, the default output
/// switched) is reopened in the background and the gap meanwhile is silence;
/// it never stops the recording.
class AudioSource {
public:
    AudioSource();
    ~AudioSource();   // Stop()
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    /// Opens the endpoint and starts capturing into an internal buffer.
    /// `device_id` is ignored for Desktop and empty means the default endpoint
    /// for Microphone. Fails with a message fit for the tray tooltip when the
    /// device is missing, microphone access is off in Windows privacy settings,
    /// or Media Foundation is not installed (Windows N without the Media
    /// Feature Pack).
    bool Start(AudioSourceKind kind, const std::wstring& device_id, std::wstring& error);

    /// Stops the thread and releases the device. Idempotent.
    void Stop();

    /// The track this source produces. Valid after a successful Start.
    [[nodiscard]] AudioTrackConfig TrackConfig() const;

    /// From now on, encode the timeline starting at QPC time `origin_seconds`
    /// into `sink`, with a fresh encoder, so the first frame delivered covers
    /// [origin, origin + 1024 / 48000). Audio captured before origin is
    /// discarded; if the device has nothing yet for that span it is silence.
    void Attach(AacFrameSink sink, double origin_seconds);

    /// Delivers everything up to QPC time `end_seconds` (padding the tail with
    /// silence to a whole frame), drains the encoder into the sink, and then
    /// stops calling it. Blocks until that is done, bounded at about a second.
    /// After it returns the sink is never called again. No-op when detached.
    void Detach(double end_seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zcr
