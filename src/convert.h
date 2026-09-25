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

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "capture.h"

namespace zcr {

struct ConvertParams {
    /// Source is FP16 scRGB (HDR) or BGRA8 sRGB-encoded (SDR).
    bool hdr = false;

    /// Brightness the cursor is drawn at in HDR, in nits. Ignored in SDR.
    float sdr_white_nits = 80.0f;

    /// Rotation of the output; the source surface is unrotated, the target is
    /// in display orientation.
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;

    bool draw_cursor = true;
};

/// Desktop surface -> P010 (4:2:0, 10 bits in the high bits of 16), all on the
/// GPU, for NVENC to read in place.
///
/// HDR: scRGB linear BT.709 -> BT.2020 linear -> nits (x80) -> PQ (ST 2084,
///      10000 nit reference) -> Y'CbCr BT.2020 non-constant luminance, 10-bit
///      limited range (Y 64..940, C 64..960).
/// SDR: sRGB-encoded BT.709 R'G'B' -> Y'CbCr BT.709, 10-bit limited range.
///
/// Two passes per frame into render target views on the target's two planes:
/// PSLuma into plane 0 (R16_UNORM, full size), PSChroma into plane 1
/// (R16G16_UNORM, half size in both directions, 2x2 box filter of the
/// non-linear R'G'B' before the matrix). Shaders are shaders/convert.hlsl,
/// compiled at build time; entry points VSMain, PSLuma, PSChroma.
///
/// Single-threaded: every call from the capture thread.
class Converter {
public:
    Converter();
    ~Converter();
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    bool Init(ID3D11Device* device, std::wstring& error);
    void Shutdown();

    /// Converts `source` (with the cursor from `cursor` when params.draw_cursor)
    /// into `target`, a DXGI_FORMAT_P010 texture created with
    /// D3D11_BIND_RENDER_TARGET. The target's size is the encoded size: the
    /// rotated source size rounded down to even. RTVs are created per target
    /// on first use and cached, so a fixed ring costs nothing after warm-up.
    ///
    /// Issues GPU work on the immediate context and returns; no CPU wait.
    bool Convert(ID3D11ShaderResourceView* source, UINT source_width, UINT source_height,
                 const CursorState& cursor, ID3D11Texture2D* target,
                 const ConvertParams& params, std::wstring& error);

    /// Drops cached RTVs, e.g. when the ring is recreated for a new segment.
    void ForgetTargets();

private:
    // Shaders, constant buffer and rasterizer state; defined in convert.cpp.
    struct Pipeline;
    std::unique_ptr<Pipeline> pipeline_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    struct TargetViews {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> luma;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> chroma;
        UINT width = 0;
        UINT height = 0;
    };
    std::unordered_map<ID3D11Texture2D*, TargetViews> targets_;
};

} // namespace zcr
