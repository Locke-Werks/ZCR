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

#include <string_view>

namespace zcr::console {

/// zcr.exe is a GUI-subsystem executable so that starting the tray never opens a
/// console. That also means its control commands have nowhere to print.
/// Borrowing the caller's console, or its redirected handles, gives --start,
/// --status and the rest somewhere to report.

/// Prints to the calling shell if there is one and stays SILENT otherwise.
/// There is deliberately no dialog fallback: ZCR never shows a window.
void Write(std::wstring_view message, bool error = false);

/// Flushes and releases the borrowed console. Safe to call when nothing was
/// ever attached.
void Detach();

} // namespace zcr::console
