ZCR records a monitor to HDR10 HEVC from the tray, with frames kept on the GPU from capture to encode.

New in 0.2.0: opt-in audio. Desktop audio and the microphone can each be turned on from the tray menu, and each is recorded to its own AAC track. You can pick a specific microphone. Audio is off by default and the choice is saved between launches.

- `ZCR-Setup.exe` installs for the current user, no administrator rights needed. It installs to `%LOCALAPPDATA%\Programs\ZCR`, and can add a Start Menu shortcut, add ZCR to your PATH and start it at sign-in. The installer doesn't start the tray. Open ZCR from the Start Menu, or it starts at your next sign-in.
- `zcr.exe` is the same program as a single portable file. Run it from anywhere.

Requires Windows 11 and an NVIDIA GPU with 10-bit HEVC encode (GTX 10 series or newer) driving the monitor being recorded.
