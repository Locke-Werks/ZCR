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
#include "capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "win32.h"

namespace zcr {
namespace {

using Microsoft::WRL::ComPtr;

/// Upper bound on the whole first-frame wait in Open.
constexpr ULONGLONG kFirstFrameWaitMs = 500;

/// Re-duplication attempts while access is lost. DuplicateOutput1 costs a few
/// milliseconds, and the lock screen can last hours, so this is a poll, not a
/// spin.
constexpr ULONGLONG kRetryIntervalMs = 250;

/// Bound on frames drained per Poll. Draining normally ends at the first
/// timeout; the cap only exists so a pathological source cannot turn a
/// non-blocking call into an unbounded one.
constexpr int kMaxDrain = 8;

[[nodiscard]] std::wstring HexHr(HRESULT hr)
{
    wchar_t buffer[16];
    std::swprintf(buffer, 16, L"0x%08X", static_cast<unsigned>(hr));
    return buffer;
}

[[nodiscard]] bool IsDeviceLoss(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
        || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

/// Text for the failures DuplicateOutput1 is known to return, written for the
/// log a user will read rather than for a developer with the headers open.
[[nodiscard]] std::wstring DescribeDuplicateError(HRESULT hr)
{
    switch (hr) {
    case E_ACCESSDENIED:
    case DXGI_ERROR_ACCESS_DENIED:
        return L"Desktop Duplication access denied: a secure desktop (UAC prompt, lock "
               L"screen, Ctrl+Alt+Del) is showing; retry later";
    case DXGI_ERROR_UNSUPPORTED:
        return L"Desktop Duplication unsupported on this output (the output is scanned out "
               L"by a different GPU than the capture device, or the process is not "
               L"per-monitor DPI aware)";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
        return L"Desktop Duplication not currently available: too many applications are "
               L"already duplicating this output";
    case DXGI_ERROR_SESSION_DISCONNECTED:
        return L"Desktop Duplication unavailable: the session is disconnected";
    case DXGI_ERROR_ACCESS_LOST:
        return L"Desktop Duplication access lost (mode change or desktop switch)";
    default:
        return L"DuplicateOutput1 failed (" + HexHr(hr) + L")";
    }
}

/// The adapter a D3D11 device was created on, by LUID. Desktop Duplication
/// only works on the adapter that scans the output out, so this is compared
/// against the monitor's before anything else is attempted.
[[nodiscard]] bool DeviceAdapterLuid(ID3D11Device* device, LUID& luid)
{
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    luid = desc.AdapterLuid;
    return true;
}

[[nodiscard]] bool SameLuid(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

/// Finds the output by GDI name on the adapter with `luid`, through a new
/// factory each time: a factory's output list is a snapshot, and after a mode
/// change or re-plug the old one describes a desktop that no longer exists.
[[nodiscard]] HRESULT FindOutputByName(const LUID& luid, const std::wstring& device_name,
                                       ComPtr<IDXGIOutput5>& out, DXGI_OUTPUT_DESC1& desc_out)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc{};
        if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))
            || !desc.AttachedToDesktop || !EqualsNoCase(desc.DeviceName, device_name)) {
            continue;
        }
        hr = output6.As(&out);
        if (FAILED(hr)) {
            return hr;
        }
        desc_out = desc;
        return S_OK;
    }
    return DXGI_ERROR_NOT_FOUND;
}

/// As FindOutputByName, falling back to the monitor's device path when the GDI
/// name has moved: \\.\DISPLAYn is reassigned on re-plug, the path is not.
[[nodiscard]] HRESULT FindOutput(const MonitorInfo& monitor, ComPtr<IDXGIOutput5>& out,
                                 DXGI_OUTPUT_DESC1& desc_out)
{
    HRESULT hr = FindOutputByName(monitor.adapter_luid, monitor.device_name, out, desc_out);
    if (hr != DXGI_ERROR_NOT_FOUND || monitor.device_path.empty()) {
        return hr;
    }
    for (const auto& candidate : EnumerateMonitors()) {
        if (EqualsNoCase(candidate.device_path, monitor.device_path)
            && SameLuid(candidate.adapter_luid, monitor.adapter_luid)
            && !EqualsNoCase(candidate.device_name, monitor.device_name)) {
            return FindOutputByName(monitor.adapter_luid, candidate.device_name, out, desc_out);
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

[[nodiscard]] HRESULT Duplicate(ID3D11Device* device, IDXGIOutput5* output,
                                ComPtr<IDXGIOutputDuplication>& out)
{
    // Order is preference. DWM hands back FP16 scRGB when the output is in HDR
    // and BGRA8 otherwise; it never converts HDR down to 8 bits for us, which is
    // the point of asking for both.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
    return output->DuplicateOutput1(device, 0, static_cast<UINT>(std::size(formats)), formats,
                                    out.ReleaseAndGetAddressOf());
}

[[nodiscard]] bool IsQuarterTurn(DXGI_MODE_ROTATION rotation)
{
    return rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
}

/// Rotates a width x height BGRA image, given in desktop orientation, into the
/// orientation of the unrotated duplication surface, so the compositor can blend
/// the cursor without knowing about rotation at all. The mapping is the inverse
/// of the one DXGI_MODE_ROTATION applies for display: desktop (dx, dy) comes
/// from surface (dy, Wd-1-dx) at 90, (Wd-1-dx, Hd-1-dy) at 180 and
/// (Hd-1-dy, dx) at 270.
[[nodiscard]] std::vector<uint32_t> RotateToSurface(const std::vector<uint32_t>& in, int w, int h,
                                                    DXGI_MODE_ROTATION rotation)
{
    if (rotation != DXGI_MODE_ROTATION_ROTATE90 && rotation != DXGI_MODE_ROTATION_ROTATE180
        && rotation != DXGI_MODE_ROTATION_ROTATE270) {
        return in;
    }
    std::vector<uint32_t> out(in.size());
    for (int v = 0; v < h; ++v) {
        for (int u = 0; u < w; ++u) {
            const uint32_t texel = in[static_cast<size_t>(v) * w + u];
            size_t index = 0;
            switch (rotation) {
            case DXGI_MODE_ROTATION_ROTATE90:   // out is h wide, w tall
                index = static_cast<size_t>(w - 1 - u) * h + v;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:  // out is w wide, h tall
                index = static_cast<size_t>(h - 1 - v) * w + (w - 1 - u);
                break;
            default:                            // 270: out is h wide, w tall
                index = static_cast<size_t>(u) * h + (h - 1 - v);
                break;
            }
            out[index] = texel;
        }
    }
    return out;
}

} // namespace

DesktopCapture::~DesktopCapture()
{
    Close();
}

void DesktopCapture::Close()
{
    dupl_.Reset();
    output_.Reset();
    surface_srv_.Reset();
    surface_.Reset();
    context_.Reset();
    device_.Reset();
    format_ = DXGI_FORMAT_UNKNOWN;
    width_ = 0;
    height_ = 0;
    rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    cursor_ = CursorState{};
    monitor_ = MonitorInfo{};
    lost_since_ = 0;
    shape_buffer_.clear();
    mode_width_ = 0;
    mode_height_ = 0;
    mode_format_ = DXGI_FORMAT_UNKNOWN;
    last_retry_ = 0;
    mode_changed_ = false;
    fatal_ = false;
    pointer_x_ = 0;
    pointer_y_ = 0;
    shape_width_ = 0;
    shape_height_ = 0;
}

bool DesktopCapture::Open(ID3D11Device* device, const MonitorInfo& monitor, std::wstring& error)
{
    Close();
    error.clear();
    if (!device) {
        error = L"no D3D11 device";
        return false;
    }

    LUID device_luid{};
    if (!DeviceAdapterLuid(device, device_luid)) {
        error = L"cannot query the capture device's adapter";
        return false;
    }
    if (!SameLuid(device_luid, monitor.adapter_luid)) {
        error = L"monitor is not driven by the capture device's adapter";
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    monitor_ = monitor;

    DXGI_OUTPUT_DESC1 output_desc{};
    HRESULT hr = FindOutput(monitor_, output_, output_desc);
    if (FAILED(hr)) {
        error = (hr == DXGI_ERROR_NOT_FOUND)
            ? L"monitor " + monitor.device_name + L" is no longer attached to the desktop"
            : L"cannot enumerate outputs (" + HexHr(hr) + L")";
        Close();
        return false;
    }

    hr = Duplicate(device_.Get(), output_.Get(), dupl_);
    if (FAILED(hr)) {
        error = DescribeDuplicateError(hr);
        Close();
        return false;
    }

    DXGI_OUTDUPL_DESC dupl_desc{};
    dupl_->GetDesc(&dupl_desc);
    mode_width_ = dupl_desc.ModeDesc.Width;
    mode_height_ = dupl_desc.ModeDesc.Height;
    mode_format_ = dupl_desc.ModeDesc.Format;
    rotation_ = dupl_desc.Rotation;

    // Desktop Duplication always delivers the whole desktop as its first frame,
    // usually within a few milliseconds. The surface is created from that
    // frame's texture so its size and format are the ones DWM really uses.
    const ULONGLONG deadline = GetTickCount64() + kFirstFrameWaitMs;
    while (!surface_) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            break;
        }
        const UINT wait = static_cast<UINT>(std::min<ULONGLONG>(deadline - now, 100));
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = dupl_->AcquireNextFrame(wait, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            error = (hr == DXGI_ERROR_ACCESS_LOST)
                ? DescribeDuplicateError(hr)
                : L"AcquireNextFrame failed (" + HexHr(hr) + L")";
            Close();
            return false;
        }
        bool copied = false;
        const HRESULT process_hr = ProcessFrame(info, resource.Get(), copied);
        resource.Reset();
        const HRESULT release_hr = dupl_->ReleaseFrame();
        if (FAILED(process_hr) || FAILED(release_hr)) {
            error = L"first frame failed (" + HexHr(FAILED(process_hr) ? process_hr : release_hr) + L")";
            Close();
            return false;
        }
    }

    if (!surface_) {
        // No frame in time. Size the surface from the output instead and start
        // black; the next desktop change fills it.
        const RECT& r = output_desc.DesktopCoordinates;
        UINT w = static_cast<UINT>(r.right - r.left);
        UINT h = static_cast<UINT>(r.bottom - r.top);
        if (IsQuarterTurn(rotation_)) {
            std::swap(w, h);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // ModeDesc.Format has matched the frame texture format in testing, but
        // it is only documented as the mode's format, so anything outside the
        // two requested formats falls back to what the HDR state implies.
        desc.Format = mode_format_;
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            desc.Format = monitor_.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&desc, nullptr, &surface_);
        if (SUCCEEDED(hr)) {
            hr = device_->CreateShaderResourceView(surface_.Get(), nullptr, &surface_srv_);
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(hr)) {
            hr = device_->CreateRenderTargetView(surface_.Get(), nullptr, &rtv);
        }
        if (FAILED(hr)) {
            error = L"cannot create the capture surface (" + HexHr(hr) + L")";
            Close();
            return false;
        }
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(rtv.Get(), black);
        format_ = desc.Format;
        width_ = desc.Width;
        height_ = desc.Height;
    }

    return true;
}

HRESULT DesktopCapture::ProcessFrame(const DXGI_OUTDUPL_FRAME_INFO& info, IDXGIResource* resource,
                                     bool& copied)
{
    // Shape first: GetFramePointerShape is only valid while the frame is held.
    if (info.PointerShapeBufferSize > 0) {
        shape_buffer_.resize(info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        HRESULT hr = dupl_->GetFramePointerShape(static_cast<UINT>(shape_buffer_.size()),
                                                 shape_buffer_.data(), &required, &shape);
        if (FAILED(hr)) {
            return hr;
        }
        hr = UpdateCursorShape(shape);
        if (FAILED(hr)) {
            return hr;
        }
    }

    // LastMouseUpdateTime is zero when only the image changed, and then
    // PointerPosition is stale and must not be read.
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursor_.visible = info.PointerPosition.Visible != FALSE;
        pointer_x_ = info.PointerPosition.Position.x;
        pointer_y_ = info.PointerPosition.Position.y;
        UpdateCursorPlacement();
    }

    // A zero LastPresentTime means the frame carries only a pointer update and
    // the texture holds nothing new.
    if (info.LastPresentTime.QuadPart == 0 || !resource) {
        return S_OK;
    }

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_TEXTURE2D_DESC src{};
    texture->GetDesc(&src);

    if (!surface_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = src.Width;
        desc.Height = src.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // Render target as well so Open can clear it when no frame arrives; it
        // costs nothing and keeps one creation path for both cases.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&desc, nullptr, &surface_);
        if (SUCCEEDED(hr)) {
            hr = device_->CreateShaderResourceView(surface_.Get(), nullptr, &surface_srv_);
        }
        if (FAILED(hr)) {
            surface_.Reset();
            surface_srv_.Reset();
            return hr;
        }
        format_ = src.Format;
        width_ = src.Width;
        height_ = src.Height;
        UpdateCursorPlacement();
    } else if (src.Width != width_ || src.Height != height_ || src.Format != format_) {
        // DWM switched under us without an ACCESS_LOST in between. Keep the
        // last image and let the caller rebuild the segment.
        mode_changed_ = true;
        return S_OK;
    }

    context_->CopyResource(surface_.Get(), texture.Get());
    copied = true;
    return S_OK;
}

HRESULT DesktopCapture::UpdateCursorShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape)
{
    const int w = static_cast<int>(shape.Width);
    const bool mono = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    // Monochrome shapes carry the AND mask and the XOR mask stacked, so DXGI
    // reports twice the real height.
    const int h = static_cast<int>(mono ? shape.Height / 2 : shape.Height);
    const size_t pitch = shape.Pitch;
    if (w <= 0 || h <= 0 || pitch == 0) {
        return S_OK;
    }
    const size_t needed = pitch * static_cast<size_t>(mono ? h * 2 : h);
    if (shape_buffer_.size() < needed) {
        return S_OK;  // malformed shape; keep the previous one
    }

    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
    CursorKind kind = CursorKind::Color;
    const unsigned char* data = shape_buffer_.data();

    switch (shape.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // Straight-alpha BGRA, used as is.
        for (int y = 0; y < h; ++y) {
            std::memcpy(&pixels[static_cast<size_t>(y) * w], data + y * pitch,
                        static_cast<size_t>(w) * 4);
        }
        kind = CursorKind::Color;
        break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        // DXGI's definition (and the Microsoft DesktopDuplication sample): mask
        // alpha 0xFF means XOR the RGB with the screen, 0 means replace. That is
        // already the Masked encoding; only stray alpha values are snapped.
        for (int y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(data + y * pitch);
            for (int x = 0; x < w; ++x) {
                const uint32_t p = row[x];
                pixels[static_cast<size_t>(y) * w + x] =
                    (p & 0x00FFFFFFu) | (((p >> 24) != 0) ? 0xFF000000u : 0u);
            }
        }
        kind = CursorKind::Masked;
        break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // out = (screen AND and_bit) XOR xor_bit, per the GDI cursor rules.
        // AND=1 keeps the screen, so it becomes the XOR case (a=255) with the
        // XOR colour; AND=0 clears it, so it becomes a replace (a=0) with the
        // XOR colour, black or white.
        for (int y = 0; y < h; ++y) {
            const unsigned char* and_row = data + y * pitch;
            const unsigned char* xor_row = data + (static_cast<size_t>(y) + h) * pitch;
            for (int x = 0; x < w; ++x) {
                const unsigned char bit = static_cast<unsigned char>(0x80u >> (x & 7));
                const bool and_set = (and_row[x >> 3] & bit) != 0;
                const bool xor_set = (xor_row[x >> 3] & bit) != 0;
                pixels[static_cast<size_t>(y) * w + x] =
                    (and_set ? 0xFF000000u : 0u) | (xor_set ? 0x00FFFFFFu : 0u);
            }
        }
        kind = CursorKind::Masked;
        break;

    default:
        return S_OK;  // unknown type; keep the previous shape
    }

    pixels = RotateToSurface(pixels, w, h, rotation_);
    const int tex_w = IsQuarterTurn(rotation_) ? h : w;
    const int tex_h = IsQuarterTurn(rotation_) ? w : h;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(tex_w);
    desc.Height = static_cast<UINT>(tex_h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels.data();
    init.SysMemPitch = static_cast<UINT>(tex_w) * 4;

    // A new texture per shape, never an update in place: the compositor may
    // still hold the old view for a frame it has queued.
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    HRESULT hr = device_->CreateTexture2D(&desc, &init, &texture);
    if (SUCCEEDED(hr)) {
        hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
    }
    if (FAILED(hr)) {
        return hr;
    }

    cursor_.shape = std::move(view);
    cursor_.kind = kind;
    cursor_.width = tex_w;
    cursor_.height = tex_h;
    shape_width_ = w;
    shape_height_ = h;
    UpdateCursorPlacement();
    return S_OK;
}

void DesktopCapture::UpdateCursorPlacement()
{
    // PointerPosition.Position is the top-left of the shape bitmap relative to
    // the output's top-left, in desktop (rotated) orientation, with the hotspot
    // already subtracted: the DesktopDuplication sample draws the shape at it
    // directly, and tests/capture_test.cpp checks it against a known cursor
    // position with a centred hotspot. Only rotation is left to undo here.
    const int surf_w = static_cast<int>(width_);
    const int surf_h = static_cast<int>(height_);
    const int desk_w = IsQuarterTurn(rotation_) ? surf_h : surf_w;
    const int desk_h = IsQuarterTurn(rotation_) ? surf_w : surf_h;
    const int px = pointer_x_;
    const int py = pointer_y_;
    const int sw = shape_width_;
    const int sh = shape_height_;

    switch (rotation_) {
    case DXGI_MODE_ROTATION_ROTATE90:
        cursor_.x = py;
        cursor_.y = desk_w - sw - px;
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        cursor_.x = desk_w - sw - px;
        cursor_.y = desk_h - sh - py;
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        cursor_.x = desk_h - sh - py;
        cursor_.y = px;
        break;
    default:
        cursor_.x = px;
        cursor_.y = py;
        break;
    }
}

CaptureStatus DesktopCapture::Fail(HRESULT hr, const wchar_t* during)
{
    dupl_.Reset();

    if (IsDeviceLoss(hr)) {
        const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
        error_ = std::wstring(L"GPU device lost during ") + during + L" (" + HexHr(hr)
            + L", removed reason " + HexHr(reason) + L")";
        fatal_ = true;
        return CaptureStatus::Fatal;
    }

    // Everything else (ACCESS_LOST, INVALID_CALL after a loss, secure desktop,
    // a failed shape texture) is treated as transient. Re-duplicating is the
    // cure for all of them, and giving up would end a recording that the next
    // unlock would have resumed.
    if (lost_since_ == 0) {
        lost_since_ = GetTickCount64();
    }
    last_retry_ = GetTickCount64();
    cursor_.visible = false;
    error_ = (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL)
        ? std::wstring(L"Desktop Duplication access lost during ") + during
        : std::wstring(during) + L" failed (" + HexHr(hr) + L")";
    return CaptureStatus::Recovering;
}

CaptureStatus DesktopCapture::TryRecover()
{
    const ULONGLONG now = GetTickCount64();
    if (now - last_retry_ < kRetryIntervalMs) {
        return CaptureStatus::Recovering;
    }
    last_retry_ = now;

    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        return Fail(removed, L"recovery");
    }

    // The output is looked up again because a mode change or a monitor that
    // went to sleep (DisplayPort drops the output entirely) invalidates the old
    // one. Not finding it is still Recovering: the monitor may wake up.
    ComPtr<IDXGIOutput5> output;
    DXGI_OUTPUT_DESC1 output_desc{};
    HRESULT hr = FindOutput(monitor_, output, output_desc);
    if (FAILED(hr)) {
        error_ = (hr == DXGI_ERROR_NOT_FOUND)
            ? L"waiting for monitor " + monitor_.device_name + L" to return"
            : L"cannot enumerate outputs (" + HexHr(hr) + L")";
        return CaptureStatus::Recovering;
    }

    ComPtr<IDXGIOutputDuplication> dupl;
    hr = Duplicate(device_.Get(), output.Get(), dupl);
    if (FAILED(hr)) {
        if (IsDeviceLoss(hr)) {
            return Fail(hr, L"DuplicateOutput1");
        }
        error_ = DescribeDuplicateError(hr);
        return CaptureStatus::Recovering;
    }

    DXGI_OUTDUPL_DESC desc{};
    dupl->GetDesc(&desc);
    output_ = std::move(output);
    dupl_ = std::move(dupl);

    if (desc.ModeDesc.Width != mode_width_ || desc.ModeDesc.Height != mode_height_
        || desc.ModeDesc.Format != mode_format_ || desc.Rotation != rotation_) {
        mode_changed_ = true;
        error_ = L"display mode changed";
        return CaptureStatus::ModeChanged;
    }

    lost_since_ = 0;
    error_.clear();
    return CaptureStatus::NoChange;
}

CaptureStatus DesktopCapture::Poll()
{
    if (!device_) {
        error_ = L"capture is not open";
        return CaptureStatus::Fatal;
    }
    if (fatal_) {
        return CaptureStatus::Fatal;
    }
    if (mode_changed_) {
        return CaptureStatus::ModeChanged;
    }
    if (!dupl_) {
        const CaptureStatus status = TryRecover();
        if (status != CaptureStatus::NoChange) {
            return status;
        }
    }

    bool copied = false;
    for (int i = 0; i < kMaxDrain; ++i) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dupl_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            break;
        }
        if (FAILED(hr)) {
            return Fail(hr, L"AcquireNextFrame");
        }

        // Released straight away whatever happens: while the frame is held DWM
        // cannot hand the next one to anybody, including the next Poll.
        const HRESULT process_hr = ProcessFrame(info, resource.Get(), copied);
        resource.Reset();
        const HRESULT release_hr = dupl_->ReleaseFrame();
        if (FAILED(process_hr)) {
            return Fail(process_hr, L"frame processing");
        }
        if (FAILED(release_hr)) {
            return Fail(release_hr, L"ReleaseFrame");
        }
        if (mode_changed_) {
            error_ = L"display mode changed";
            return CaptureStatus::ModeChanged;
        }
    }
    return copied ? CaptureStatus::NewFrame : CaptureStatus::NoChange;
}

} // namespace zcr
