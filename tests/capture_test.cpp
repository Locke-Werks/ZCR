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

// Standalone harness for monitors.cpp and capture.cpp, not part of the CMake
// build. Compile with src/monitors.cpp, src/capture.cpp and src/win32.cpp and
// link d3d11 dxgi user32 gdi32 shell32 ole32.
//
// It enumerates the monitors, opens Desktop Duplication on the primary one,
// polls at 120 Hz for five seconds while animating a small window so DWM has
// something to present, checks where DXGI puts the pointer against a cursor
// with a centred hotspot, and writes one captured frame to
// tests/out/capture_test.bmp (or the directory given as argv[1]).

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture.h"
#include "monitors.h"
#include "win32.h"

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* FormatName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16_FLOAT";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return L"R10G10B10A2_UNORM";
    default: return L"other";
    }
}

const wchar_t* StatusName(zcr::CaptureStatus s)
{
    switch (s) {
    case zcr::CaptureStatus::NewFrame: return L"NewFrame";
    case zcr::CaptureStatus::NoChange: return L"NoChange";
    case zcr::CaptureStatus::Recovering: return L"Recovering";
    case zcr::CaptureStatus::ModeChanged: return L"ModeChanged";
    case zcr::CaptureStatus::Fatal: return L"Fatal";
    }
    return L"?";
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            // Subnormal: value = mant * 2^-24.
            const float v = static_cast<float>(mant) * (1.0f / 16777216.0f);
            return sign ? -v : v;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

uint8_t SrgbEncode(float linear)
{
    linear = std::clamp(linear, 0.0f, 1.0f);
    const float v = (linear <= 0.0031308f) ? linear * 12.92f
                                           : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

bool WriteBmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgr)
{
    const int row = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = w;
    info.biHeight = -h;  // top-down
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(row * h);
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        return false;
    }
    std::fwrite(&file, sizeof(file), 1, f);
    std::fwrite(&info, sizeof(info), 1, f);
    std::vector<uint8_t> line(static_cast<size_t>(row), 0);
    for (int y = 0; y < h; ++y) {
        std::memcpy(line.data(), &bgr[static_cast<size_t>(y) * w * 3], static_cast<size_t>(w) * 3);
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}

/// Copies the capture surface to the CPU and writes it, box-downscaled so the
/// long side is at most 1920, as an 8-bit BMP. FP16 scRGB is tone mapped the
/// crude way: SDR white maps to 1.0, everything above clips.
bool DumpSurface(ID3D11Device* device, const zcr::DesktopCapture& capture, float sdr_white_nits,
                 const std::wstring& path)
{
    D3D11_TEXTURE2D_DESC desc{};
    capture.Surface()->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        return false;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ctx->CopyResource(staging.Get(), capture.Surface());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const int sw = static_cast<int>(desc.Width);
    const int sh = static_cast<int>(desc.Height);
    const int factor = std::max(1, (std::max(sw, sh) + 1919) / 1920);
    const int dw = sw / factor;
    const int dh = sh / factor;
    const bool fp16 = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const float scale = 80.0f / sdr_white_nits;

    std::vector<uint8_t> bgr(static_cast<size_t>(dw) * dh * 3);
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            float acc[3] = {0, 0, 0};  // r g b, linear for fp16, encoded for 8-bit
            for (int j = 0; j < factor; ++j) {
                const uint8_t* rowp = base + static_cast<size_t>(y * factor + j) * mapped.RowPitch;
                for (int i = 0; i < factor; ++i) {
                    const int sx = x * factor + i;
                    if (fp16) {
                        const auto* p = reinterpret_cast<const uint16_t*>(rowp) + sx * 4;
                        acc[0] += HalfToFloat(p[0]);
                        acc[1] += HalfToFloat(p[1]);
                        acc[2] += HalfToFloat(p[2]);
                    } else {
                        const uint8_t* p = rowp + sx * 4;
                        acc[0] += p[2];
                        acc[1] += p[1];
                        acc[2] += p[0];
                    }
                }
            }
            const float n = static_cast<float>(factor * factor);
            uint8_t* out = &bgr[(static_cast<size_t>(y) * dw + x) * 3];
            for (int c = 0; c < 3; ++c) {
                const float v = acc[c] / n;
                out[2 - c] = fp16 ? SrgbEncode(v * scale)
                                  : static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
        }
    }
    ctx->Unmap(staging.Get(), 0);
    return WriteBmp(path, dw, dh, bgr);
}

// A small window whose colour changes every tick, so DWM presents during the
// polling run even on an otherwise idle desktop, and whose class cursor is the
// crosshair: its hotspot is in the middle, which is what distinguishes "top-left
// of the shape" from "hotspot" in PointerPosition.
int g_tick = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const int v = (g_tick * 7) & 0xFF;
        HBRUSH brush = CreateSolidBrush(RGB(v, 255 - v, 128));
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::vector<zcr::MonitorInfo> monitors = zcr::EnumerateMonitors();
    std::wprintf(L"%zu monitor(s)\n", monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::wprintf(L"[%zu] %ls  %ls%ls\n", i, m.friendly_name.c_str(), m.device_name.c_str(),
                     m.primary ? L"  (primary)" : L"");
        std::wprintf(L"     path     %ls\n", m.device_path.c_str());
        std::wprintf(L"     rect     %ld,%ld %dx%d  rotation %d\n", m.desktop_rect.left,
                     m.desktop_rect.top, zcr::Width(m.desktop_rect), zcr::Height(m.desktop_rect),
                     static_cast<int>(m.rotation));
        std::wprintf(L"     adapter  [%u.%u] %ls  nvidia=%d  luid %08lX:%08lX\n", m.adapter_index,
                     m.output_index, m.adapter_name.c_str(), m.is_nvidia ? 1 : 0,
                     static_cast<unsigned long>(m.adapter_luid.HighPart), m.adapter_luid.LowPart);
        std::wprintf(L"     hdr=%d  sdr white %.1f nits\n", m.hdr ? 1 : 0, m.sdr_white_nits);
        const auto& h = m.hdr_meta;
        std::wprintf(L"     primaries R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) W(%.4f,%.4f)\n",
                     h.red_x, h.red_y, h.green_x, h.green_y, h.blue_x, h.blue_y, h.white_x,
                     h.white_y);
        std::wprintf(L"     mastering %.4f..%.1f nits  MaxCLL %u  MaxFALL %u\n", h.min_mastering_nits,
                     h.max_mastering_nits, h.max_cll, h.max_fall);
    }

    const zcr::MonitorInfo* monitor = zcr::ResolveMonitor(monitors, L"");
    if (!monitor) {
        std::wprintf(L"no monitors\n");
        return 1;
    }
    std::wprintf(L"\ncapturing %ls (%ls), Windows HDR %ls\n", monitor->friendly_name.c_str(),
                 monitor->device_name.c_str(), monitor->hdr ? L"ON" : L"off");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
        || FAILED(factory->EnumAdapterByLuid(monitor->adapter_luid, IID_PPV_ARGS(&adapter)))) {
        std::wprintf(L"cannot find the monitor's adapter\n");
        return 1;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        std::wprintf(L"D3D11CreateDevice failed 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    zcr::DesktopCapture capture;
    std::wstring error;
    const ULONGLONG open_start = GetTickCount64();
    if (!capture.Open(device.Get(), *monitor, error)) {
        std::wprintf(L"Open failed: %ls\n", error.c_str());
        return 1;
    }
    std::wprintf(L"Open took %llu ms: %ux%u %ls rotation %d  (Hdr()=%d, monitor hdr=%d: %ls)\n",
                 GetTickCount64() - open_start, capture.Width(), capture.Height(),
                 FormatName(capture.Format()), static_cast<int>(capture.Rotation()),
                 capture.Hdr() ? 1 : 0, monitor->hdr ? 1 : 0,
                 capture.Hdr() == monitor->hdr ? L"consistent" : L"MISMATCH");

    // Cursor probe window, centred on the captured monitor.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // --mono swaps in a 1bpp AND/XOR cursor built here, one quadrant per mask
    // combination, to exercise the MONOCHROME expansion. The default Windows 11
    // scheme has no monochrome cursors left to test it with.
    bool mono = false;
    for (int i = 1; i < argc; ++i) {
        mono = mono || std::wstring(argv[i]) == L"--mono";
    }
    HCURSOR mono_cursor = nullptr;
    if (mono) {
        // 32x32, rows of 4 bytes. Top half AND=0 (replace), bottom half AND=1
        // (keep screen); left half XOR=0, right half XOR=1. So the quadrants
        // are TL black, TR white, BL transparent, BR invert.
        BYTE and_plane[32 * 4];
        BYTE xor_plane[32 * 4];
        for (int y = 0; y < 32; ++y) {
            for (int b = 0; b < 4; ++b) {
                and_plane[y * 4 + b] = (y >= 16) ? 0xFF : 0x00;
                xor_plane[y * 4 + b] = (b >= 2) ? 0xFF : 0x00;
            }
        }
        mono_cursor = CreateCursor(GetModuleHandleW(nullptr), 16, 16, 32, 32, and_plane, xor_plane);
    }
    wc.hCursor = mono_cursor ? mono_cursor : LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = L"ZcrCaptureTest";
    RegisterClassExW(&wc);
    const RECT& mr = monitor->desktop_rect;
    const int cx = mr.left + zcr::Width(mr) / 2;
    const int cy = mr.top + zcr::Height(mr) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"zcr",
                                WS_POPUP | WS_VISIBLE, cx - 150, cy - 150, 300, 300, nullptr,
                                nullptr, wc.hInstance, nullptr);
    POINT saved_cursor{};
    GetCursorPos(&saved_cursor);
    SetCursorPos(cx, cy);
    Pump();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const LONGLONG period = freq.QuadPart / 120;

    int counts[5] = {};
    int shape_changes = 0;
    ID3D11ShaderResourceView* last_shape = nullptr;
    double worst_poll_ms = 0.0;
    int polls = 0;
    for (LONGLONG next = start.QuadPart + period;; next += period) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= 5 * freq.QuadPart) {
            break;
        }
        if (next > now.QuadPart) {
            LARGE_INTEGER due;
            due.QuadPart = -((next - now.QuadPart) * 10000000 / freq.QuadPart);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }

        ++g_tick;
        // Keep the pointer on the window: WM_SETCURSOR only fires on movement.
        if (g_tick % 60 == 1) {
            SetCursorPos(cx + ((g_tick / 60) & 1), cy);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        Pump();

        LARGE_INTEGER a;
        LARGE_INTEGER b;
        QueryPerformanceCounter(&a);
        const zcr::CaptureStatus status = capture.Poll();
        QueryPerformanceCounter(&b);
        worst_poll_ms = std::max(worst_poll_ms,
                                 static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart);
        ++polls;
        ++counts[static_cast<int>(status)];
        if (status != zcr::CaptureStatus::NewFrame && status != zcr::CaptureStatus::NoChange) {
            std::wprintf(L"Poll -> %ls: %ls\n", StatusName(status), capture.LastError().c_str());
        }
        if (capture.Cursor().shape.Get() != last_shape) {
            last_shape = capture.Cursor().shape.Get();
            ++shape_changes;
        }
    }
    CloseHandle(timer);

    std::wprintf(L"\n%d polls in 5 s: NewFrame %d (%.1f fps), NoChange %d, Recovering %d, "
                 L"ModeChanged %d, Fatal %d; cursor shape changes %d; worst Poll %.3f ms\n",
                 polls, counts[0], counts[0] / 5.0, counts[1], counts[2], counts[3], counts[4],
                 shape_changes, worst_poll_ms);

    // Pointer semantics. The crosshair's hotspot is at its centre, so if
    // PointerPosition were the hotspot the offset below would be (0,0); if it is
    // the shape's top-left, the offset is the hotspot.
    POINT pos{};
    GetCursorPos(&pos);
    const zcr::CursorState& cur = capture.Cursor();
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    ICONINFO ii{};
    int hot_x = -1;
    int hot_y = -1;
    if (GetCursorInfo(&ci) && ci.hCursor && GetIconInfo(ci.hCursor, &ii)) {
        hot_x = static_cast<int>(ii.xHotspot);
        hot_y = static_cast<int>(ii.yHotspot);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    const int rel_x = pos.x - mr.left;
    const int rel_y = pos.y - mr.top;
    std::wprintf(L"cursor: visible=%d kind=%ls shape %dx%d at (%d,%d); real pointer at (%d,%d) "
                 L"output-relative; offset (%d,%d); GetIconInfo hotspot (%d,%d)\n",
                 cur.visible ? 1 : 0, cur.kind == zcr::CursorKind::Color ? L"Color" : L"Masked",
                 cur.width, cur.height, cur.x, cur.y, rel_x, rel_y, rel_x - cur.x, rel_y - cur.y,
                 hot_x, hot_y);

    // Read the normalized shape back and print the texel at each quadrant's
    // centre. For --mono the expected BGRA words are TL 00000000 (black
    // replace), TR 00FFFFFF (white replace), BL FF000000 (transparent),
    // BR FFFFFFFF (invert).
    if (cur.shape) {
        ComPtr<ID3D11Resource> res;
        cur.shape->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        D3D11_TEXTURE2D_DESC sd{};
        tex->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &st))) {
            ctx->CopyResource(st.Get(), tex.Get());
            if (SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                auto at = [&](UINT x, UINT y) {
                    return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData)
                                                             + y * m.RowPitch)[x];
                };
                const UINT qx = sd.Width / 4;
                const UINT qy = sd.Height / 4;
                std::wprintf(L"shape quadrants: TL %08X TR %08X BL %08X BR %08X\n", at(qx, qy),
                             at(3 * qx, qy), at(qx, 3 * qy), at(3 * qx, 3 * qy));
                ctx->Unmap(st.Get(), 0);
            }
        }
    }

    DestroyWindow(hwnd);
    if (mono_cursor) {
        DestroyCursor(mono_cursor);
    }
    SetCursorPos(saved_cursor.x, saved_cursor.y);
    Pump();

    // Let DWM present the desktop without the probe window before dumping.
    for (int i = 0; i < 50; ++i) {
        Sleep(10);
        Pump();
        (void)capture.Poll();
    }

    std::wstring dir = L"tests\\out";
    bool switch_desktop = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--switch-desktop") {
            switch_desktop = true;
        } else if (std::wstring(argv[i]) == L"--mono") {
            continue;
        } else {
            dir = argv[i];
        }
    }
    zcr::EnsureDirectory(dir);
    const std::wstring path = zcr::JoinPath(dir, L"capture_test.bmp");
    if (DumpSurface(device.Get(), capture, monitor->sdr_white_nits, path)) {
        std::wprintf(L"wrote %ls\n", path.c_str());
    } else {
        std::wprintf(L"frame dump failed\n");
        return 1;
    }

    if (switch_desktop) {
        // Exercises the ACCESS_LOST path the way a UAC prompt does: switch the
        // input desktop away for a second, then back, polling throughout and
        // printing every status transition.
        HDESK original = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        HDESK scratch = CreateDesktopW(L"ZcrCaptureTest", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!original || !scratch) {
            std::wprintf(L"cannot create a scratch desktop (%lu)\n", GetLastError());
        } else {
            zcr::CaptureStatus last = zcr::CaptureStatus::NoChange;
            const ULONGLONG t0 = GetTickCount64();
            auto poll_for = [&](ULONGLONG ms) {
                const ULONGLONG until = GetTickCount64() + ms;
                while (GetTickCount64() < until) {
                    const zcr::CaptureStatus s = capture.Poll();
                    const bool quiet = s == zcr::CaptureStatus::NewFrame || s == zcr::CaptureStatus::NoChange;
                    const bool last_quiet = last == zcr::CaptureStatus::NewFrame
                        || last == zcr::CaptureStatus::NoChange;
                    if (quiet != last_quiet || (!quiet && s != last)) {
                        std::wprintf(L"  +%llu ms %ls  %ls\n", GetTickCount64() - t0, StatusName(s),
                                     quiet ? L"" : capture.LastError().c_str());
                    }
                    last = s;
                    Sleep(8);
                }
            };
            SwitchDesktop(scratch);
            poll_for(1000);
            SwitchDesktop(original);
            poll_for(3000);
            std::wprintf(L"after desktop switch: last status %ls\n", StatusName(last));
        }
        if (scratch) CloseDesktop(scratch);
        if (original) CloseDesktop(original);
    }

    capture.Close();
    return 0;
}
