ZCR records a monitor to HDR10 HEVC from the tray, with frames kept on the GPU from capture to encode.

- `ZCR-Setup.exe` installs for the current user, no administrator rights needed. It installs to `%LOCALAPPDATA%\Programs\ZCR`, and can add a Start Menu shortcut, add ZCR to your PATH and start it at sign-in. The installer doesn't start the tray. Open ZCR from the Start Menu, or it starts at your next sign-in.
- `zcr.exe` is the same program as a single portable file. Run it from anywhere.

Requires Windows 11 and an NVIDIA GPU with 10-bit HEVC encode (GTX 10 series or newer) driving the monitor being recorded.
