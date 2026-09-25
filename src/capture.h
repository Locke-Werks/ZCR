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
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "monitors.h"

namespace zcr {

/// How the cursor texture is to be composited. Desktop Duplication hands back
/// three pointer shape types; capture.cpp normalizes them into two.
enum class CursorKind {
    /// DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR. BGRA with straight alpha:
    /// out = lerp(desktop, rgb, a).
    Color,

    /// MONOCHROME and MASKED_COLOR, normalized. Per texel:
    ///   a == 0   -> out = rgb (opaque replace)
    ///   a == 255 -> out = desktop XOR rgb (rgb black = transparent,
    ///               rgb white = invert)
    /// Monochrome AND/XOR masks are expanded into this encoding: AND=0,XOR=c ->
    /// a=0,rgb=c; AND=1,XOR=c -> a=255,rgb=c.
    Masked,
};

struct CursorState {
    bool visible = false;

    /// Top-left of the shape texture in the captured surface's own (unrotated)
    /// pixel coordinates, hotspot already subtracted. May be negative or run
    /// off the edge; the shader clips.
    int x = 0;
    int y = 0;

    int width = 0;
    int height = 0;   // for monochrome, the real height (DXGI reports double)
    CursorKind kind = CursorKind::Color;

    /// DXGI_FORMAT_B8G8R8A8_UNORM, width x height. Null until the first shape
    /// arrives. Replaced (not updated in place) when the shape changes, so a
    /// view held across a Poll stays valid for the frame it was taken for.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shape;
};

enum class CaptureStatus {
    NewFrame,    // Surface() holds a newer desktop image than before
    NoChange,    // nothing new since the last Poll (cursor may still have moved)
    Recovering,  // access lost (UAC, lock screen, mode change); Surface() keeps
                 // the last good image, keep polling
    ModeChanged, // came back with a different size, format or rotation: the
                 // caller must close the segment and re-open
    Fatal,       // cannot continue; LastError() says why
};

/// Desktop Duplication of one output into a texture ZCR owns.
///
/// Uses IDXGIOutput5::DuplicateOutput1 with {R16G16B16A16_FLOAT,
/// B8G8R8A8_UNORM}: in HDR the desktop arrives as FP16 scRGB, in SDR as BGRA8.
/// Each new frame is CopyResource'd (VRAM to VRAM) into Surface() and the
/// duplication frame is released immediately, so DWM is never held up by the
/// encoder and the last image is always available for repeat frames.
///
/// Single-threaded: every call from the capture thread.
class DesktopCapture {
public:
    DesktopCapture() = default;
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    /// `device` must have been created on `monitor`'s adapter. Blocks up to
    /// ~500 ms waiting for the first frame so Surface() is valid on return.
    bool Open(ID3D11Device* device, const MonitorInfo& monitor, std::wstring& error);
    void Close();

    /// Never blocks: AcquireNextFrame(0), draining to the newest frame.
    /// Pointer position and shape updates are applied to Cursor() as they
    /// arrive, whether or not the desktop image changed.
    CaptureStatus Poll();

    [[nodiscard]] ID3D11Texture2D* Surface() const { return surface_.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* SurfaceSrv() const { return surface_srv_.Get(); }

    /// R16G16B16A16_FLOAT (HDR, scRGB linear, 1.0 = 80 nits) or
    /// B8G8R8A8_UNORM (SDR, sRGB-encoded).
    [[nodiscard]] DXGI_FORMAT Format() const { return format_; }
    [[nodiscard]] bool Hdr() const { return format_ == DXGI_FORMAT_R16G16B16A16_FLOAT; }

    /// Surface size, unrotated (as DXGI hands it over).
    [[nodiscard]] UINT Width() const { return width_; }
    [[nodiscard]] UINT Height() const { return height_; }
    [[nodiscard]] DXGI_MODE_ROTATION Rotation() const { return rotation_; }

    [[nodiscard]] const CursorState& Cursor() const { return cursor_; }
    [[nodiscard]] const std::wstring& LastError() const { return error_; }

private:
    // capture.cpp owns the rest; it may add members freely.
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput5> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> surface_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> surface_srv_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    CursorState cursor_;
    std::wstring error_;
    MonitorInfo monitor_;
    ULONGLONG lost_since_ = 0;
    std::vector<unsigned char> shape_buffer_;

    // Added by capture.cpp.
    HRESULT ProcessFrame(const DXGI_OUTDUPL_FRAME_INFO& info, IDXGIResource* resource,
                         bool& copied);
    HRESULT UpdateCursorShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape);
    void UpdateCursorPlacement();
    CaptureStatus Fail(HRESULT hr, const wchar_t* during);
    CaptureStatus TryRecover();

    UINT mode_width_ = 0;          // DXGI_OUTDUPL_DESC::ModeDesc at Open
    UINT mode_height_ = 0;
    DXGI_FORMAT mode_format_ = DXGI_FORMAT_UNKNOWN;
    ULONGLONG last_retry_ = 0;
    bool mode_changed_ = false;    // sticky until Close
    bool fatal_ = false;           // sticky until Close
    int pointer_x_ = 0;            // DXGI PointerPosition, desktop orientation
    int pointer_y_ = 0;
    int shape_width_ = 0;          // shape size, desktop orientation
    int shape_height_ = 0;
};

} // namespace zcr
