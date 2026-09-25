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
#include <dxgi.h>   // DXGI_MODE_ROTATION

#include <string>
#include <vector>

#include "video_format.h"

namespace zcr {

/// One attached, active display output as DXGI and DisplayConfig see it.
struct MonitorInfo {
    /// GDI device name, e.g. \\.\DISPLAY1. Not stable across reboots or
    /// re-plugs; use device_path to remember a choice.
    std::wstring device_name;

    /// Monitor device interface path from DISPLAYCONFIG_TARGET_DEVICE_NAME
    /// (monitorDevicePath). Stable for a given physical monitor on a given port,
    /// which is why settings store this rather than the index or the GDI name.
    std::wstring device_path;

    /// Human-readable, e.g. "DELL U3224KB". Falls back to device_name.
    std::wstring friendly_name;

    HMONITOR hmonitor = nullptr;
    RECT desktop_rect{};      // physical pixels, virtual-desktop coordinates
    bool primary = false;

    /// DXGI_OUTPUT_DESC1::ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
    /// i.e. Windows HDR is on for this output right now.
    bool hdr = false;

    /// DXGI_OUTPUT_DESC1::Rotation. Desktop Duplication hands back the
    /// unrotated surface, so the converter needs this.
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;

    /// The adapter that scans this output out. Desktop Duplication only works on
    /// that adapter, and ZCR only encodes on NVIDIA, so is_nvidia gates
    /// everything.
    LUID adapter_luid{};
    UINT adapter_index = 0;   // IDXGIFactory1::EnumAdapters1 index
    UINT output_index = 0;    // IDXGIAdapter1::EnumOutputs index
    std::wstring adapter_name;
    bool is_nvidia = false;   // DXGI_ADAPTER_DESC1::VendorId == 0x10DE

    /// SDR content brightness in HDR mode, in nits: DISPLAYCONFIG_SDR_WHITE_LEVEL
    /// SDRWhiteLevel / 1000 * 80. 80 when it cannot be queried or HDR is off.
    float sdr_white_nits = 80.0f;

    /// From DXGI_OUTPUT_DESC1. Used as mastering display and MaxCLL/MaxFALL.
    HdrMetadata hdr_meta;
};

/// Every active output on every adapter, primary first, then in DXGI order.
/// Never throws. Empty only if DXGI itself fails.
[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();

/// Picks the monitor a setting refers to: exact device_path match, else the
/// primary, else the first. Null only when `monitors` is empty.
[[nodiscard]] const MonitorInfo* ResolveMonitor(const std::vector<MonitorInfo>& monitors,
                                                const std::wstring& device_path);

} // namespace zcr
