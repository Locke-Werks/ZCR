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

#include <cstdint>

namespace zcr {

/// Mastering display and content light level, in natural units. Each writer
/// (HEVC SEI in nvenc.cpp, mdcv/clli boxes in mp4mux.cpp) converts to its own
/// fixed-point representation, so the conversion lives next to the spec it
/// follows instead of here.
struct HdrMetadata {
    // CIE 1931 xy chromaticities of the display the desktop was captured from.
    double red_x = 0.708, red_y = 0.292;
    double green_x = 0.170, green_y = 0.797;
    double blue_x = 0.131, blue_y = 0.046;
    double white_x = 0.3127, white_y = 0.3290;

    double max_mastering_nits = 1000.0;
    double min_mastering_nits = 0.0001;

    // Unknown before the recording ends, and the header is written first, so
    // these are the display's own capabilities: peak and full-frame luminance.
    uint16_t max_cll = 1000;
    uint16_t max_fall = 400;
};

/// What a recording segment is. Fixed for the life of one file: a change in any
/// field mid-recording closes the file and starts a new segment.
struct VideoFormat {
    uint32_t width = 0;   // encoded size, always even (4:2:0)
    uint32_t height = 0;
    uint32_t fps = 60;    // 30, 60 or 120; constant frame rate
    bool hdr = false;     // true: BT.2020 / SMPTE ST 2084 (PQ) / BT.2020nc
                          // false: BT.709 / BT.709 / BT.709
    HdrMetadata hdr_meta; // meaningful only when hdr

    // ITU-T H.273 code points, as they go into the VUI and the colr box.
    [[nodiscard]] uint8_t ColourPrimaries() const { return hdr ? 9 : 1; }
    [[nodiscard]] uint8_t TransferCharacteristics() const { return hdr ? 16 : 1; }
    [[nodiscard]] uint8_t MatrixCoefficients() const { return hdr ? 9 : 1; }
    [[nodiscard]] bool FullRange() const { return false; } // always limited (tv) range
};

} // namespace zcr
