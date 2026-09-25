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

namespace zcr {

/// Local\ scopes the mutex to the interactive session: one tray per logged-on
/// user, which is also the scope of the control pipe's name.
inline constexpr wchar_t kMutexName[] = L"Local\\ZCR.Instance";

inline constexpr wchar_t kWindowClass[] = L"ZCR.Host";
inline constexpr wchar_t kWindowTitle[] = L"ZCR";

/// Followed by the session id. Pipe names are machine-wide, so without the
/// session a second logged-on user's tray would collide with the first.
inline constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\ZCR.";

// Private messages to the hidden host window.
inline constexpr UINT WM_APP_TRAY = WM_APP + 1;      // Shell_NotifyIcon callback
inline constexpr UINT WM_APP_RECORDER = WM_APP + 2;  // Recorder state changed on its own
/// Sent (never posted) by the pipe thread with an IpcRequest* in lParam, so the
/// Recorder is only ever touched on the UI thread.
inline constexpr UINT WM_APP_COMMAND = WM_APP + 3;

// Timer ids.
inline constexpr UINT_PTR kTimerTooltip = 1;   // recording clock in the tooltip

/// A double click arrives as two NIN_SELECTs. Without a dead time after each
/// toggle it would start a recording and immediately stop it again.
inline constexpr ULONGLONG kToggleDebounceMs = 400;

// Tray menu commands. TrackPopupMenuEx returns these directly, so they never
// travel as WM_COMMAND. Zero is reserved: it is what a dismissed menu returns.
inline constexpr UINT kMenuToggle = 40001;
inline constexpr UINT kMenuFps30 = 40002;
inline constexpr UINT kMenuFps60 = 40003;
inline constexpr UINT kMenuFps120 = 40004;
inline constexpr UINT kMenuCursor = 40005;
inline constexpr UINT kMenuOpenFolder = 40006;
inline constexpr UINT kMenuExit = 40007;

/// Monitor N (0-based, EnumerateMonitors order) is kMenuMonitorFirst + N.
inline constexpr UINT kMenuMonitorFirst = 41000;
inline constexpr UINT kMenuMonitorMax = 64;

} // namespace zcr
