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
// monitors.h names DXGI_MODE_ROTATION without including DXGI itself, so the
// DXGI header has to come first.
#include <dxgi1_6.h>

#include "monitors.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "win32.h"

namespace zcr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kNvidiaVendorId = 0x10DE;

/// What DisplayConfig knows about one active source that DXGI does not.
struct DisplayConfigEntry {
    std::wstring gdi_name;       // \\.\DISPLAYn, the join key with DXGI
    LUID source_adapter{};       // the adapter that owns the source
    std::wstring friendly_name;
    std::wstring device_path;
    float sdr_white_nits = 80.0f;
    bool sdr_white_known = false;
};

[[nodiscard]] bool SameLuid(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

/// QueryDisplayConfig with the documented retry: the topology can change between
/// GetDisplayConfigBufferSizes and the query, which then fails with
/// ERROR_INSUFFICIENT_BUFFER and has to be sized again.
[[nodiscard]] bool QueryActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count)
            != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        const LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                               &mode_count, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) {
            paths.resize(path_count);
            modes.resize(mode_count);
            return true;
        }
        if (status != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] std::vector<DisplayConfigEntry> QueryDisplayConfigEntries()
{
    std::vector<DisplayConfigEntry> entries;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActivePaths(paths, modes)) {
        return entries;
    }

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }

        // In clone mode several targets share one source. The first one wins:
        // the capture is of the source, and one name is as good as another.
        const std::wstring gdi_name = source.viewGdiDeviceName;
        const bool seen = std::any_of(entries.begin(), entries.end(),
                                      [&](const DisplayConfigEntry& e) {
                                          return EqualsNoCase(e.gdi_name, gdi_name);
                                      });
        if (seen) {
            continue;
        }

        DisplayConfigEntry entry;
        entry.gdi_name = gdi_name;
        entry.source_adapter = path.sourceInfo.adapterId;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            entry.friendly_name = target.monitorFriendlyDeviceName;
            entry.device_path = target.monitorDevicePath;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) {
            // SDRWhiteLevel is a multiplier on 80 nits in thousandths: 1000 is
            // 80 nits, the scRGB 1.0 reference.
            entry.sdr_white_nits = static_cast<float>(white.SDRWhiteLevel / 1000.0 * 80.0);
            entry.sdr_white_known = true;
        }

        entries.push_back(std::move(entry));
    }
    return entries;
}

[[nodiscard]] const DisplayConfigEntry* FindEntry(const std::vector<DisplayConfigEntry>& entries,
                                                  const std::wstring& gdi_name)
{
    for (const auto& entry : entries) {
        if (EqualsNoCase(entry.gdi_name, gdi_name)) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] uint16_t NitsToU16(double nits, uint16_t fallback)
{
    if (!(nits >= 1.0)) {  // also rejects NaN
        return fallback;
    }
    return static_cast<uint16_t>(std::min(std::lround(nits), 65535L));
}

[[nodiscard]] bool ValidChromaticity(float x, float y)
{
    return x > 0.0f && x < 1.0f && y > 0.0f && y < 1.0f;
}

/// Fills HdrMetadata from what the driver reports for the panel (its EDID, as
/// Windows parsed it). Anything missing or nonsensical keeps the BT.2020 / 1000
/// nit defaults, because a zero in an SEI or mdcv box reads as "no light at all"
/// to some players rather than as "unknown".
[[nodiscard]] HdrMetadata MetadataFrom(const DXGI_OUTPUT_DESC1& desc)
{
    HdrMetadata meta;

    const bool primaries_ok = ValidChromaticity(desc.RedPrimary[0], desc.RedPrimary[1])
        && ValidChromaticity(desc.GreenPrimary[0], desc.GreenPrimary[1])
        && ValidChromaticity(desc.BluePrimary[0], desc.BluePrimary[1]);
    if (primaries_ok) {
        meta.red_x = desc.RedPrimary[0];
        meta.red_y = desc.RedPrimary[1];
        meta.green_x = desc.GreenPrimary[0];
        meta.green_y = desc.GreenPrimary[1];
        meta.blue_x = desc.BluePrimary[0];
        meta.blue_y = desc.BluePrimary[1];
    }
    if (ValidChromaticity(desc.WhitePoint[0], desc.WhitePoint[1])) {
        meta.white_x = desc.WhitePoint[0];
        meta.white_y = desc.WhitePoint[1];
    }

    meta.max_cll = NitsToU16(desc.MaxLuminance, 1000);
    meta.max_fall = NitsToU16(desc.MaxFullFrameLuminance, 400);
    // Full-frame above peak is a driver error, and a MaxFALL above MaxCLL is
    // rejected outright by some validators.
    meta.max_fall = std::min(meta.max_fall, meta.max_cll);

    meta.max_mastering_nits = static_cast<double>(meta.max_cll);
    if (desc.MinLuminance > 0.0f && desc.MinLuminance < meta.max_mastering_nits) {
        meta.min_mastering_nits = desc.MinLuminance;
    }
    return meta;
}

[[nodiscard]] bool IsPrimary(HMONITOR monitor)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    return monitor && GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

} // namespace

std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> found;

    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
        return found;
    }
    // Factory6 is only asked for so a pre-1803 system fails here, in one place,
    // rather than later at DuplicateOutput1 with a less obvious error.
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(factory1.As(&factory))) {
        return found;
    }

    const std::vector<DisplayConfigEntry> config = QueryDisplayConfigEntries();

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc))
            || (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) {
                continue;
            }
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(output6->GetDesc1(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }

            MonitorInfo info;
            info.device_name = desc.DeviceName;
            info.hmonitor = desc.Monitor;
            info.desktop_rect = desc.DesktopCoordinates;
            info.primary = IsPrimary(desc.Monitor);
            info.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            info.rotation = desc.Rotation;
            info.adapter_luid = adapter_desc.AdapterLuid;
            info.adapter_index = a;
            info.output_index = o;
            info.adapter_name = adapter_desc.Description;
            info.is_nvidia = adapter_desc.VendorId == kNvidiaVendorId;
            // With HDR off DXGI reports the panel's SDR-ish gamut here. That is
            // harmless: hdr_meta is only read when hdr is true.
            info.hdr_meta = MetadataFrom(desc);

            const DisplayConfigEntry* entry = FindEntry(config, info.device_name);
            if (entry) {
                info.device_path = entry->device_path;
                info.friendly_name = entry->friendly_name;
                if (info.hdr && entry->sdr_white_known) {
                    info.sdr_white_nits = entry->sdr_white_nits;
                }
            }
            if (info.friendly_name.empty()) {
                info.friendly_name = info.device_name;
            }

            // Hybrid systems can list one output under two adapters. The one
            // DisplayConfig names as the source's adapter is the one that scans
            // it out, and the only one Desktop Duplication will work on.
            auto existing = std::find_if(found.begin(), found.end(), [&](const MonitorInfo& m) {
                return EqualsNoCase(m.device_name, info.device_name);
            });
            if (existing != found.end()) {
                const bool new_owns = entry && SameLuid(entry->source_adapter, info.adapter_luid);
                const bool old_owns = entry && SameLuid(entry->source_adapter, existing->adapter_luid);
                if (new_owns && !old_owns) {
                    *existing = std::move(info);
                }
                continue;
            }
            found.push_back(std::move(info));
        }
    }

    std::stable_partition(found.begin(), found.end(),
                          [](const MonitorInfo& m) { return m.primary; });
    return found;
}

const MonitorInfo* ResolveMonitor(const std::vector<MonitorInfo>& monitors,
                                  const std::wstring& device_path)
{
    if (monitors.empty()) {
        return nullptr;
    }
    if (!device_path.empty()) {
        for (const auto& monitor : monitors) {
            if (EqualsNoCase(monitor.device_path, device_path)) {
                return &monitor;
            }
        }
    }
    for (const auto& monitor : monitors) {
        if (monitor.primary) {
            return &monitor;
        }
    }
    return &monitors.front();
}

} // namespace zcr
