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
#include "diag_log.h"

#include "win32.h"

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <mutex>
#include <vector>

namespace zcr::log {
namespace {

constexpr long long kMaxBytes = 256 * 1024;

std::mutex g_gate;
ULONGLONG g_started_at = GetTickCount64();

std::wstring ResolvePath()
{
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) {
        return {};
    }
    return JoinPath(JoinPath(std::move(dir), L"ZCR"), L"zcr.log");
}

/// Truncate rather than rotate. Nobody wants a log directory.
void TruncateIfHuge(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return;
    }

    const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32)
                           | data.nFileSizeLow;
    if (size <= kMaxBytes) {
        return;
    }

    Handle truncate(CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

} // namespace

const std::wstring& Path()
{
    static const std::wstring path = ResolvePath();
    return path;
}

void Write(std::wstring_view message)
{
    const std::wstring& path = Path();
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_gate);

    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash))) {
        return;
    }

    TruncateIfHuge(path);

    // FILE_APPEND_DATA without GENERIC_WRITE is the atomic-append access mask:
    // every write lands at the end even if another process holds the file open.
    Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t prefix[64];
    _snwprintf_s(prefix, _TRUNCATE, L"%02u:%02u:%02u.%03u +%6llums  ",
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                 GetTickCount64() - g_started_at);

    std::wstring line(prefix);
    line.append(message);
    line.append(L"\r\n");

    const std::string utf8 = Narrow(line);

    // A UTF-8 BOM on a brand new file, so Notepad does not guess the encoding
    // wrong on the first non-ASCII path we log.
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.Get(), &size) && size.QuadPart == 0) {
        static constexpr char kBom[] = "\xEF\xBB\xBF";
        DWORD written = 0;
        WriteFile(file.Get(), kBom, 3, &written, nullptr);
    }

    DWORD written = 0;
    WriteFile(file.Get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void Writef(const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);

    // Measure, then format. A truncated diagnostic is worse than a slow one.
    const int needed = _vscwprintf(format, args);
    va_end(args);

    if (needed < 0) {
        return;
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);

    va_start(args, format);
    const int written = vswprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    Write(std::wstring_view(buffer.data(), static_cast<size_t>(written)));
}

const wchar_t* Describe(int tristate)
{
    if (tristate > 0) {
        return L"yes";
    }
    if (tristate == 0) {
        return L"no";
    }
    return L"unknown";
}

std::wstring Abbrev(std::wstring_view text, size_t max)
{
    const size_t keep = (std::min)(text.size(), max);

    std::wstring out;
    out.reserve(keep + 24);
    for (const wchar_t c : text.substr(0, keep)) {
        out.push_back((c == L'\r' || c == L'\n') ? L' ' : c);
    }
    if (text.size() > keep) {
        out += L"... (+" + std::to_wstring(text.size() - keep) + L" chars)";
    }
    return out;
}

void WriteBanner()
{
    Write(std::wstring(72, L'-'));
    Writef(L"ZCR %hs starting", ZCR_VERSION_STRING);
    Writef(L"  exe            %s", ExePath().c_str());

    OSVERSIONINFOEXW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    // RtlGetVersion is the only version query that is not shimmed by the
    // compatibility layer. GetVersionExW would lie about the build number.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            rtl_get_version(reinterpret_cast<PRTL_OSVERSIONINFOW>(&os));
            Writef(L"  os             %lu.%lu.%lu",
                   os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
        }
    }
}

} // namespace zcr::log
