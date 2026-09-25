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
#include "settings.h"

#include "json.h"
#include "win32.h"

#include <algorithm>
#include <vector>

namespace zcr {
namespace {

constexpr uint32_t kMinCq = 1;
constexpr uint32_t kMaxCq = 51;

/// Far above anything NVENC HEVC produces at 8K120, so it only rejects typos.
constexpr uint32_t kMaxMbps = 2000;

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash))) {
        return false;
    }

    // Temp file and rename, so an interrupted save cannot leave half a file.
    const std::wstring temp = path + L".tmp";
    {
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return false;
        }

        const std::string utf8 = Narrow(text);
        DWORD written = 0;
        if (!utf8.empty()
            && (!WriteFile(file.Get(), utf8.data(), static_cast<DWORD>(utf8.size()),
                           &written, nullptr)
                || written != utf8.size())) {
            return false;
        }
    }

    return MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

/// Hand-edited numbers can be negative or absurd; anything out of range keeps
/// the default rather than being clamped into a value nobody asked for.
bool ReadUint(const json::Object& object, std::wstring_view key, uint32_t lo,
              uint32_t hi, uint32_t& out, std::wstring& detail)
{
    const json::Value* value = json::Find(object, key);
    if (!value) {
        return true;
    }
    if (value->type != json::Value::Type::Number || value->number < lo
        || value->number > hi) {
        detail += L" \"" + std::wstring(key) + L"\" must be a number from "
                  + std::to_wstring(lo) + L" to " + std::to_wstring(hi) + L";";
        return false;
    }
    out = static_cast<uint32_t>(value->number);
    return true;
}

void ReadString(const json::Object& object, std::wstring_view key, std::wstring& out,
                std::wstring& detail)
{
    const json::Value* value = json::Find(object, key);
    if (!value) {
        return;
    }
    if (value->type == json::Value::Type::String) {
        out = value->string;
    } else if (value->type == json::Value::Type::Null) {
        out.clear();
    } else {
        detail += L" \"" + std::wstring(key) + L"\" must be a string;";
    }
}

void ReadBool(const json::Object& object, std::wstring_view key, bool& out,
              std::wstring& detail)
{
    const json::Value* value = json::Find(object, key);
    if (!value) {
        return;
    }
    if (value->type == json::Value::Type::Bool) {
        out = value->boolean;
    } else {
        detail += L" \"" + std::wstring(key) + L"\" must be true or false;";
    }
}

std::wstring ExpandEnvironment(const std::wstring& text)
{
    if (text.find(L'%') == std::wstring::npos) {
        return text;
    }
    const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0) {
        return text;
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD written = ExpandEnvironmentStringsW(text.c_str(), buffer.data(), needed);
    if (written == 0 || written > needed) {
        return text;
    }
    return std::wstring(buffer.data());
}

} // namespace

std::wstring Settings::FilePath()
{
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) {
        return {};
    }
    return JoinPath(JoinPath(std::move(dir), L"ZCR"), L"zcr.json");
}

bool Settings::IsSupportedFps(uint32_t fps)
{
    return fps == 30 || fps == 60 || fps == 120;
}

Settings Settings::Load(std::wstring& detail)
{
    detail.clear();
    Settings settings;

    const std::wstring path = FilePath();
    if (path.empty()) {
        detail = L"no LocalAppData folder, using defaults";
        return settings;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return settings;   // first run: defaults, and nothing written until asked
    }

    std::wstring text;
    if (!ReadUtf8File(path, text)) {
        detail = L"could not read " + path + L", using defaults";
        return settings;
    }

    json::Object object;
    std::wstring error;
    if (!json::ParseObject(text, object, error)) {
        detail = path + L": " + error + L"; using defaults and leaving the file alone";
        return settings;
    }

    // Each key is taken on its own, so one bad value costs that value only.
    std::wstring problems;
    uint32_t fps = settings.fps;
    if (ReadUint(object, L"fps", 1, 1000, fps, problems)) {
        if (IsSupportedFps(fps)) {
            settings.fps = fps;
        } else {
            problems += L" \"fps\" must be 30, 60 or 120;";
        }
    }
    ReadString(object, L"monitor", settings.monitor, problems);
    ReadBool(object, L"cursor", settings.cursor, problems);
    ReadString(object, L"output_dir", settings.output_dir, problems);
    ReadUint(object, L"cq", kMinCq, kMaxCq, settings.cq, problems);
    ReadUint(object, L"max_mbps", 0, kMaxMbps, settings.max_mbps, problems);

    if (!problems.empty()) {
        problems.pop_back();   // the trailing ';'
        detail = path + L":" + problems + L". Those keys use their defaults.";
    }
    return settings;
}

bool Settings::Save(std::wstring& error) const
{
    const std::wstring path = FilePath();
    if (path.empty()) {
        error = L"no LocalAppData folder to save into";
        return false;
    }

    // Replacing a file that does not parse would silently throw away what the
    // person wrote in it, so keep the old one next to the new one.
    std::wstring existing;
    if (ReadUtf8File(path, existing)) {
        json::Object ignored;
        std::wstring parse_error;
        if (!json::ParseObject(existing, ignored, parse_error)) {
            CopyFileW(path.c_str(), (path + L".bad").c_str(), FALSE);
        }
    }

    std::wstring text;
    text += L"{\r\n";
    text += L"  \"fps\": " + std::to_wstring(fps) + L",\r\n";
    text += L"  \"monitor\": " + json::Quote(monitor) + L",\r\n";
    text += L"  \"cursor\": " + std::wstring(cursor ? L"true" : L"false") + L",\r\n";
    text += L"  \"output_dir\": " + json::Quote(output_dir) + L",\r\n";
    text += L"  \"cq\": " + std::to_wstring(cq) + L",\r\n";
    text += L"  \"max_mbps\": " + std::to_wstring(max_mbps) + L"\r\n";
    text += L"}\r\n";

    if (!WriteTextFile(path, text)) {
        const DWORD code = GetLastError();
        error = L"could not write " + path + L" (error " + std::to_wstring(code) + L")";
        return false;
    }
    return true;
}

std::wstring Settings::ResolvedOutputDir() const
{
    const std::wstring configured(Trim(output_dir));
    if (!configured.empty()) {
        return ExpandEnvironment(configured);
    }

    // Stored empty rather than resolved, so a Videos folder moved to another
    // drive later is followed instead of pinned at its old location.
    std::wstring videos = KnownFolder(FOLDERID_Videos);
    if (videos.empty()) {
        videos = KnownFolder(FOLDERID_Profile);
    }
    return JoinPath(std::move(videos), L"ZCR");
}

RecorderSettings Settings::ToRecorder() const
{
    RecorderSettings out;
    out.fps = IsSupportedFps(fps) ? fps : 60;
    out.monitor_device_path = monitor;
    out.cursor = cursor;
    out.output_dir = ResolvedOutputDir();
    out.cq = std::clamp(cq, kMinCq, kMaxCq);
    out.max_mbps = (std::min)(max_mbps, kMaxMbps);
    return out;
}

} // namespace zcr
