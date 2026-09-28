ZCR records a monitor to HDR10 HEVC from the tray, with frames kept on the GPU from capture to encode.

New in 0.3.0: recordings keep full-resolution color (HEVC 4:4:4) by default, so colored text and thin colored lines stay crisp instead of smearing, with HDR unchanged. The new Chroma menu switches back to 4:2:0 for files that play on browsers, phones and TVs. 4:4:4 needs a player or editor that decodes HEVC 4:4:4, such as mpv, VLC or DaVinci Resolve.

New in 0.2.2: audio stays in step through a stall where a game holds the GPU for a second or more, through a sleep that lands at an awkward moment, and through an audio encoder restart.

New in 0.2.1: fixes for 0.2.0's audio. A slow disk no longer leaves gaps in the audio, a sleep during a recording no longer throws audio out of step with the picture, and a failed write reports its real cause.

New in 0.2.0: opt-in audio. Desktop audio and the microphone can each be turned on from the tray menu, and each is recorded to its own AAC track. You can pick a specific microphone. Audio is off by default and the choice is saved between launches.

- `ZCR-Setup.exe` installs for the current user, no administrator rights needed. It installs to `%LOCALAPPDATA%\Programs\ZCR`, and can add a Start Menu shortcut, add ZCR to your PATH and start it at sign-in. The installer doesn't start the tray. Open ZCR from the Start Menu, or it starts at your next sign-in.
- `zcr.exe` is the same program as a single portable file. Run it from anywhere.

Requires Windows 11 and an NVIDIA GPU with 10-bit HEVC encode (GTX 10 series or newer) driving the monitor being recorded.
