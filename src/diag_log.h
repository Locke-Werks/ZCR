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

#include <string>
#include <string_view>

namespace zcr {

/// Append-only diagnostic log.
///
/// The tray shows no dialogs and no toasts, so when a recording fails the icon
/// turns amber and this file says why: which adapter, which DXGI or NVENC
/// status, which frame.
///
/// Best effort throughout: logging must never be the reason a recording fails,
/// so every function here swallows its own failures.
namespace log {

/// %LOCALAPPDATA%\ZCR\zcr.log
[[nodiscard]] const std::wstring& Path();

void Write(std::wstring_view message);

/// printf-style, for the call sites that would otherwise build a string by hand.
void Writef(const wchar_t* format, ...);

/// Called once at startup so every log opens with the environment it ran in.
void WriteBanner();

/// "yes" / "no" / "unknown", for the tri-state elevation queries.
[[nodiscard]] const wchar_t* Describe(int tristate);

/// One line of at most `max` characters from a command line or a prompt: line
/// breaks become spaces and anything past the limit is replaced by a count, so
/// a briefing pasted into a command cannot turn the log into a transcript.
[[nodiscard]] std::wstring Abbrev(std::wstring_view text, size_t max = 160);

} // namespace log
} // namespace zcr
