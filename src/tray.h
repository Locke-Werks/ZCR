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

/// Runs the tray instance until Exit or `zcr --quit`: a hidden host window, the
/// notification-area icon, the Recorder, and the control pipe.
///
/// The icon color is the only feedback the product gives: gray idle, red
/// recording, amber failed with the reason in the tooltip. No balloons, no
/// toasts, no dialogs.
///
/// The caller must already hold the single-instance mutex. Returns the process
/// exit code.
[[nodiscard]] int RunTray(HINSTANCE instance);

} // namespace zcr
