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
#include "autostart.h"

#include "win32.h"

namespace zcr::autostart {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"ZCR";

std::wstring CurrentUserName()
{
    wchar_t buffer[256];
    DWORD size = ARRAYSIZE(buffer);
    if (GetUserNameW(buffer, &size) && size > 0) {
        return std::wstring(buffer, size - 1);
    }
    return L"this user";
}

} // namespace

bool Register(std::wstring& message)
{
    const std::wstring exe = ExePath();
    if (exe.empty()) {
        message = L"could not determine this executable's path";
        return false;
    }

    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        message = L"could not open HKCU\\" + std::wstring(kRunKey) + L" (error "
                  + std::to_wstring(status) + L")";
        return false;
    }

    // Quoted, because the install path contains a space and an unquoted Run
    // value would be split at it.
    const std::wstring value = L"\"" + exe + L"\"";
    status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        message = L"could not write the Run value (error " + std::to_wstring(status) + L")";
        return false;
    }

    message = L"registered autostart for " + CurrentUserName() + L": " + exe;
    return true;
}

bool Unregister(std::wstring& message)
{
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) {
        message = L"no Run key, nothing to remove";
        return true;
    }
    if (status != ERROR_SUCCESS) {
        message = L"could not open HKCU\\" + std::wstring(kRunKey) + L" (error "
                  + std::to_wstring(status) + L")";
        return false;
    }

    status = RegDeleteValueW(key, kValueName);
    RegCloseKey(key);

    if (status == ERROR_FILE_NOT_FOUND) {
        message = L"autostart was not registered";
        return true;
    }
    if (status != ERROR_SUCCESS) {
        message = L"could not remove autostart (error " + std::to_wstring(status) + L")";
        return false;
    }

    message = L"removed autostart for " + CurrentUserName();
    return true;
}

bool IsRegistered()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return false;
    }

    const LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, nullptr,
                                            nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

} // namespace zcr::autostart
