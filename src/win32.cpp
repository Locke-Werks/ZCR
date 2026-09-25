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
#include "win32.h"

#include <tlhelp32.h>

#include <algorithm>
#include <vector>

namespace zcr {

std::wstring ExePath()
{
    // The manifest is longPathAware, so the path is not bounded by MAX_PATH.
    // Grow until it fits rather than truncating, which GetModuleFileNameW does
    // silently while still reporting the buffer size as the length.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::wstring(buffer.data(), written);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ExeDir()
{
    std::wstring path = ExePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash);
    return path;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    CoTaskMem out;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, out.Receive()))) {
        return {};
    }
    return out.Get() ? std::wstring(out.Get()) : std::wstring{};
}

std::wstring JoinPath(std::wstring base, std::wstring_view leaf)
{
    if (base.empty()) {
        return std::wstring(leaf);
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    base.push_back(L'\\');
    base.append(leaf);
    return base;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadUtf8File(const std::wstring& path, std::wstring& out)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart > (8 << 20)) {
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!bytes.empty()
        && !ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                     &read, nullptr)) {
        return false;
    }
    bytes.resize(read);

    // Strip a UTF-8 BOM. System.Text.Json consumed it silently, so any config
    // that has been through Notepad carries one and would otherwise fail on the
    // invisible U+FEFF before the first key.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    out = Widen(bytes);
    return true;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // Walk down creating parents. SHCreateDirectoryExW would do this in one call
    // but is capped at MAX_PATH, and the manifest promises long paths work.
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0) {
        EnsureDirectory(path.substr(0, slash));
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ClassNameOf(HWND hwnd)
{
    // 256 is the documented ceiling for a registered class name.
    wchar_t buffer[256];
    const int written = GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer));
    if (written <= 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<size_t>(written));
}

std::wstring ProcessImageName(DWORD pid)
{
    if (pid == 0) {
        return {};
    }

    // QueryFullProcessImageNameW needs a handle we may not be able to open
    // across integrity levels. The toolhelp snapshot reports the image name for
    // every process regardless, which is all the caller wants.
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return {};
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) {
        return {};
    }

    do {
        if (entry.th32ProcessID == pid) {
            return std::wstring(entry.szExeFile);
        }
    } while (Process32NextW(snapshot.Get(), &entry));

    return {};
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    // CompareStringOrdinal is the correct call for identifiers: it is a true
    // ordinal fold, so it does not vary with the user's locale the way
    // lstrcmpiW does. A Turkish locale really does break a naive tolower on "I".
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

std::wstring ToLower(std::wstring s)
{
    if (s.empty()) {
        return s;
    }
    // In-place, invariant. CharLowerBuffW is locale-invariant for ASCII, which
    // is all the config enums use.
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring_view Trim(std::wstring_view s)
{
    const auto is_space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::wstring Widen(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
}

std::string Narrow(std::wstring_view utf16)
{
    if (utf16.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                           static_cast<int>(utf16.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace zcr
