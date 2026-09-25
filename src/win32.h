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
#include <shlobj.h>

#include <string>
#include <utility>

namespace zcr {

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

/// Owns a HANDLE closed with CloseHandle. INVALID_HANDLE_VALUE and null are both
/// treated as empty, because the Win32 API is not consistent about which one it
/// hands back on failure: CreateFile returns INVALID_HANDLE_VALUE, OpenProcess
/// returns null.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(Normalize(h)) {}

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }

    ~Handle() { Reset(); }

    void Reset(HANDLE h = nullptr)
    {
        if (h_) {
            CloseHandle(h_);
        }
        h_ = Normalize(h);
    }

    [[nodiscard]] HANDLE Get() const { return h_; }
    [[nodiscard]] bool Valid() const { return h_ != nullptr; }
    explicit operator bool() const { return Valid(); }

    /// Gives up ownership without closing. For the rare case where closing would
    /// be worse than leaking: a handle another thread is still blocked on cannot
    /// be closed safely, because the value can be reused the moment it is.
    [[nodiscard]] HANDLE Release() { return std::exchange(h_, nullptr); }

private:
    static HANDLE Normalize(HANDLE h)
    {
        return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }

    HANDLE h_ = nullptr;
};

/// Owns a block returned by SHGetKnownFolderPath, which must be freed with
/// CoTaskMemFree rather than delete or free.
class CoTaskMem {
public:
    CoTaskMem() = default;
    CoTaskMem(const CoTaskMem&) = delete;
    CoTaskMem& operator=(const CoTaskMem&) = delete;
    ~CoTaskMem() { CoTaskMemFree(p_); }

    [[nodiscard]] PWSTR* Receive() { return &p_; }
    [[nodiscard]] PCWSTR Get() const { return p_; }

private:
    PWSTR p_ = nullptr;
};

// ---------------------------------------------------------------------------
// Rects
//
// Everything in this program is PHYSICAL pixels. The process is PerMonitorV2
// from the manifest, so nothing here has been through a DPI-virtualising round
// trip, and nothing may be allowed to.
// ---------------------------------------------------------------------------

[[nodiscard]] inline int Width(const RECT& r) { return r.right - r.left; }
[[nodiscard]] inline int Height(const RECT& r) { return r.bottom - r.top; }

[[nodiscard]] inline bool IsEmptyRect(const RECT& r)
{
    return Width(r) <= 0 || Height(r) <= 0;
}

[[nodiscard]] inline bool Intersects(const RECT& a, const RECT& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

/// True when `inner` covers all of `outer`. Used to decide "is this window
/// fullscreen on this monitor", so the tolerance is a real parameter rather than
/// a magic number: see Enforcer for why it is small but not zero.
[[nodiscard]] inline bool Covers(const RECT& inner, const RECT& outer, int tolerance)
{
    return inner.left <= outer.left + tolerance
        && inner.top <= outer.top + tolerance
        && inner.right >= outer.right - tolerance
        && inner.bottom >= outer.bottom - tolerance;
}

[[nodiscard]] inline bool operator==(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// ---------------------------------------------------------------------------
// Paths and process identity
// ---------------------------------------------------------------------------

/// Full path of this executable. Empty only if GetModuleFileNameW fails, which
/// in practice does not happen for the running image.
[[nodiscard]] std::wstring ExePath();

/// Directory containing this executable, without a trailing separator.
[[nodiscard]] std::wstring ExeDir();

/// SHGetKnownFolderPath, empty on failure. Pass FOLDERID_LocalAppData and the
/// like. Never assume success: a folder can be missing on a redirected profile.
[[nodiscard]] std::wstring KnownFolder(REFKNOWNFOLDERID id);

/// Joins with a single backslash, tolerating a trailing one on `base`.
[[nodiscard]] std::wstring JoinPath(std::wstring base, std::wstring_view leaf);

[[nodiscard]] bool FileExists(const std::wstring& path);

/// Reads a UTF-8 file into UTF-16, dropping a BOM. False on any failure, which
/// callers treat as unreadable rather than as an empty file. Capped at 8 MB,
/// because nothing this program reads is larger and a file that is was not
/// written by anyone who meant it for us.
[[nodiscard]] bool ReadUtf8File(const std::wstring& path, std::wstring& out);

/// Creates every missing directory along `path`. True if the directory exists
/// afterwards, whether or not this call is what created it.
bool EnsureDirectory(const std::wstring& path);

/// Class name of a window, empty on failure. Bounded, so it cannot be used to
/// probe for an over-long name.
[[nodiscard]] std::wstring ClassNameOf(HWND hwnd);

/// Image name of the process owning `pid`, e.g. "WindowsTerminal.exe", empty if
/// it cannot be queried. Case is whatever the process table reports.
[[nodiscard]] std::wstring ProcessImageName(DWORD pid);

/// Case-insensitive comparison, for window classes, file names and config
/// enums, all of which Windows treats case-insensitively.
[[nodiscard]] bool EqualsNoCase(std::wstring_view a, std::wstring_view b);

[[nodiscard]] std::wstring ToLower(std::wstring s);

/// Trims ASCII whitespace from both ends. Config values arrive hand-edited.
[[nodiscard]] std::wstring_view Trim(std::wstring_view s);

[[nodiscard]] std::wstring Widen(std::string_view utf8);
[[nodiscard]] std::string Narrow(std::wstring_view utf16);

} // namespace zcr
