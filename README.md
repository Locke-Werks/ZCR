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
scRGB, a pixel shader converts it to BT.2020 PQ 10-bit 4:2:0, and NVENC encodes
straight from those textures. Only the finished bitstream touches system memory,
which is why recording costs a fraction of a percent of CPU.

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
- Monitor
- Show cursor
- Open recordings folder
- Exit

Framerate and monitor are locked while a recording is running. Hover the icon
for the elapsed time, or for the reason when it is amber.

Recordings go to `Videos\ZCR\ZCR_<date>_<time>.mp4`.

## What you get

- HEVC Main 10, constant frame rate, 2 second GOP, no B-frames.
- In HDR mode: BT.2020 primaries, PQ transfer, and mastering display and content
  light level metadata from the display, in both the bitstream and the MP4.
  SDR content sits at the Windows SDR brightness setting, as it does on screen.
- In SDR mode: BT.709.
- Fragmented MP4. If the process is killed or the power goes, everything up to
  the last completed 2 second fragment is still playable.
- If the resolution or HDR state changes mid-recording, the file is closed and
  recording continues in `..._part2.mp4`.
- Above 4K60, each frame is split across both NVENC engines so 4K120 keeps up.

## Command line

The same `zcr.exe` drives the running tray instance over a named pipe:

```
zcr --start [--fps N] [--monitor N]   start recording, print the output path
zcr --stop                            stop, print the finished path
zcr --toggle                          print "started <path>" or "stopped <path>"
zcr --status                          idle | recording ... | error <message>
zcr --record-for SECONDS [--fps N] [--monitor N]
zcr --list-monitors                   N for --monitor is the first column
zcr --quit                            stop any recording and exit the tray
zcr --register-autostart              start at sign-in (HKCU Run key)
zcr --unregister-autostart
```

`--start`, `--toggle` and `--record-for` launch the tray if it is not running.
`--fps` and `--monitor` change the saved settings, the same as the menu.

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
