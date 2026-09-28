<div align="center">

<img src="assets/zcr.ico" width="96" alt="ZCR">

# ZCR

**Click to record. Click to stop. HDR that comes out the way it went in.**

[![release](https://img.shields.io/github/v/release/Locke-Werks/ZCR?style=flat-square&color=d6262a)](https://github.com/Locke-Werks/ZCR/releases)
[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2011%20%7C%20NVIDIA-d6262a?style=flat-square)](#requirements)

</div>

---

ZCR (Zero Copy Recorder) records a monitor to HDR10 HEVC. It is a tray icon and
nothing else: no overlay, no toasts, no capture border, no account. The icon is
gray when idle, red while recording, and amber when something went wrong.

Frames never leave the GPU. Desktop Duplication hands over the desktop as FP16
scRGB, a pixel shader converts it to BT.2020 PQ 10-bit, and NVENC encodes
straight from those textures, at full 4:4:4 chroma by default. Only the finished bitstream touches system memory,
which is why recording costs a fraction of a percent of CPU.

Audio is the exception, and it is off unless you turn it on. Desktop audio and
the microphone are captured with WASAPI and encoded to AAC on the CPU, each
into its own track.

## Requirements

- Windows 11
- An NVIDIA GPU with 10-bit HEVC encode (GTX 10 series or newer), driving the
  monitor being recorded
- A driver recent enough for NVENC API 13.1

No other GPU vendor is supported.

## Installing

[Releases](https://github.com/Locke-Werks/ZCR/releases) has two signed builds.
`ZCR-Setup.exe` installs for the current user without administrator rights: it
copies ZCR to `%LOCALAPPDATA%\Programs\ZCR` and can add a Start Menu shortcut,
add ZCR to the PATH and start it at sign-in. `zcr.exe` is the same program as a
portable file.

## Using it

**Left click** the tray icon to start or stop.

**Right click** for:

- Framerate: 30, 60 or 120 fps
- Chroma: 4:4:4 or 4:2:0
- Monitor
- Show cursor
- Record desktop audio
- Record microphone
- Microphone: the default, or a specific device
- Open recordings folder
- Exit

Framerate, chroma, monitor and the audio choices are locked while a recording is
running. Hover the icon
for the elapsed time, or for the reason when it is amber.

Recordings go to `Videos\ZCR\ZCR_<date>_<time>.mp4`.

## What you get

- HEVC Main 4:4:4 10 by default: full-resolution color, so colored text and
  thin colored lines keep clean edges. Chroma 4:2:0 gives HEVC Main 10
  instead, which plays on more devices, including browsers, phones and TVs.
  4:4:4 needs a player or editor that decodes HEVC 4:4:4, such as mpv, VLC or
  DaVinci Resolve.
- Constant frame rate, 2 second GOP, no B-frames.
- In HDR mode: BT.2020 primaries, PQ transfer, and mastering display and content
  light level metadata from the display, in both the bitstream and the MP4.
  SDR content sits at the Windows SDR brightness setting, as it does on screen.
- In SDR mode: BT.709.
- Fragmented MP4. If the process is killed or the power goes, everything up to
  the last completed 2 second fragment is still playable.
- If the resolution or HDR state changes mid-recording, the file is closed and
  recording continues in `..._part2.mp4`.
- If the machine goes to sleep, the recording is finalized first.
- Above 4K60, each frame is split across both NVENC engines so 4K120 keeps up.
- With audio on: desktop audio as a 48 kHz stereo AAC track at 192 kbps, the
  microphone as a separate 48 kHz mono AAC track at 96 kbps, both aligned to the
  video from its first frame. Players play the first audio track; editors see
  both. Desktop audio follows the default output device if it changes
  mid-recording, and an unplugged microphone leaves silence rather than
  stopping the recording.

## Command line

The same `zcr.exe` drives the running tray instance over a named pipe:

```
zcr --start [options]                 start recording, print the output path
zcr --stop                            stop, print the finished path
zcr --toggle                          print "started <path>" or "stopped <path>"
zcr --status                          idle | recording ... | error <message>
zcr --record-for SECONDS [options]
zcr --list-monitors                   N for --monitor is the first column
zcr --list-mics                       N for --mic-device is the first column
zcr --quit                            stop any recording and exit the tray
zcr --register-autostart              start at sign-in (HKCU Run key)
zcr --unregister-autostart

options:
  --fps 30|60|120
  --chroma 444|420
  --monitor N
  --desktop-audio on|off
  --mic on|off
  --mic-device default|N
```

`--start`, `--toggle` and `--record-for` launch the tray if it is not running.
The options change the saved settings, the same as the menu.

Exit codes: 0 ok, 1 error (message on stderr), 2 not running.

`zcr.exe` is a GUI executable, so PowerShell does not wait for it unless the
output is captured: `zcr --status | Write-Output`.

## Settings

`%LOCALAPPDATA%\ZCR\zcr.json`. Comments and trailing commas are allowed.

| Key | Default | |
|---|---|---|
| `fps` | `60` | 30, 60 or 120 |
| `monitor` | primary | monitor device path, set from the menu |
| `cursor` | `true` | |
| `output_dir` | `Videos\ZCR` | `%VAR%` is expanded |
| `cq` | `24` | constant quality, 1 to 51, lower is better |
| `max_mbps` | `0` | bitrate cap; 0 scales with resolution and fps, about 100 at 4K60 |
| `chroma` | `444` | 444 or 420 |
| `desktop_audio` | `false` | record the default output device |
| `mic` | `false` | record a microphone |
| `mic_device` | default | microphone device id, set from the menu |

The log is `%LOCALAPPDATA%\ZCR\zcr.log`.

## Building

Visual Studio 2022 with the C++ workload, and CMake 3.28 or newer.

```
cmake --preset release
cmake --build --preset release
```

`scripts\install.ps1` builds, signs, copies the exe to
`%LOCALAPPDATA%\Programs\ZCR`, adds that folder to the user PATH, registers it
to start at sign-in and starts it.
Pass `-NoSign` on a machine without the signing credentials.

## License

GPLv3. `third_party/nvenc/nvEncodeAPI.h` is NVIDIA's, under the MIT license in
its header.
