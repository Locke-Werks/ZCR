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
#include "parent_console.h"

#include "win32.h"

#include <string>

namespace zcr::console {
namespace {

bool g_attached = false;

bool Attach()
{
    if (g_attached) {
        return true;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return false;
    }
    g_attached = true;
    return true;
}

/// The standard handle when the caller supplied one, otherwise the console we
/// just borrowed. Falling back to CONOUT$ matters because a GUI process started
/// from Explorer has no standard handles at all.
HANDLE StreamHandle(bool error)
{
    const HANDLE inherited = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (inherited && inherited != INVALID_HANDLE_VALUE) {
        return inherited;
    }

    static Handle conout(CreateFileW(L"CONOUT$", GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr));
    return conout.Get();
}

void Emit(std::wstring_view message, bool error)
{
    const HANDLE stream = StreamHandle(error);
    if (!stream || stream == INVALID_HANDLE_VALUE) {
        return;
    }

    std::wstring line(message);
    line.append(L"\r\n");

    // GetConsoleMode succeeds only for a real console. When the caller
    // redirected us into a file or a pipe, WriteConsoleW would fail, so the
    // bytes have to go out as UTF-8 through WriteFile instead.
    DWORD mode = 0;
    if (GetConsoleMode(stream, &mode)) {
        DWORD written = 0;
        WriteConsoleW(stream, line.data(), static_cast<DWORD>(line.size()),
                      &written, nullptr);
        return;
    }

    const std::string utf8 = Narrow(line);
    DWORD written = 0;
    WriteFile(stream, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

} // namespace

/// A caller that redirected our output (a script, a pipe, Claude's shell) hands
/// down real standard handles even though there is no console to attach to, so
/// those are used first rather than only after a successful attach.
static bool HasInheritedStream(bool error)
{
    const HANDLE h = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    return h && h != INVALID_HANDLE_VALUE;
}

void Write(std::wstring_view message, bool error)
{
    if (!HasInheritedStream(error) && !Attach()) {
        return;
    }
    Emit(message, error);
}

void Detach()
{
    if (!g_attached) {
        return;
    }
    FreeConsole();
    g_attached = false;
}

} // namespace zcr::console
