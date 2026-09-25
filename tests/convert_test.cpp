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

// Standalone check of Converter against a double-precision CPU model of the
// same color pipeline. Not part of the CMake build. Needs an NVIDIA GPU.
//
//   fxc the three entry points into <dir>\shaders\, then
//   cl /std:c++20 /EHsc /Isrc /I<dir> tests\convert_test.cpp src\convert.cpp d3d11.lib dxgi.lib
//
// Every case converts a synthetic desktop on the GPU, reads the P010 result
// back and compares every luma and chroma code with the CPU model, tolerance
// one code. Exit code 0 only if every case passes.

#include <d3d11_3.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include "convert.h"

using Microsoft::WRL::ComPtr;

namespace zcr::test {
namespace {

// ---------------------------------------------------------------------------
// CPU model, double precision, written from the specs rather than the shader.
// ---------------------------------------------------------------------------

struct Rgb {
    double r = 0, g = 0, b = 0;
};

struct Image {
    int w = 0;
    int h = 0;
    std::vector<Rgb> px;   // the exact values the GPU reads (FP16- or UNORM8-rounded)
    [[nodiscard]] const Rgb& At(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

struct CursorImage {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bgra;
    zcr::CursorKind kind = zcr::CursorKind::Color;
};

// BT.709 -> BT.2020 linear, derived here from the xy primaries so the shader's
// hard-coded BT.2087 constants are checked rather than copied.
struct Mat3 {
    double m[3][3]{};
};

Mat3 Inverse(const Mat3& a)
{
    const auto& m = a.m;
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                       - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                       + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Mat3 r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return r;
}

Mat3 Multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

Mat3 RgbToXyz(double rx, double ry, double gx, double gy, double bx, double by, double wx,
              double wy)
{
    Mat3 p;
    const double xs[3] = {rx, gx, bx};
    const double ys[3] = {ry, gy, by};
    for (int c = 0; c < 3; ++c) {
        p.m[0][c] = xs[c] / ys[c];
        p.m[1][c] = 1.0;
        p.m[2][c] = (1.0 - xs[c] - ys[c]) / ys[c];
    }
    const double white[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
    const Mat3 inv = Inverse(p);
    double s[3]{};
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            s[i] += inv.m[i][k] * white[k];
        }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            p.m[r][c] *= s[c];
        }
    }
    return p;
}

Mat3 g_709_to_2020;

Rgb Apply(const Mat3& m, const Rgb& v)
{
    return {m.m[0][0] * v.r + m.m[0][1] * v.g + m.m[0][2] * v.b,
            m.m[1][0] * v.r + m.m[1][1] * v.g + m.m[1][2] * v.b,
            m.m[2][0] * v.r + m.m[2][1] * v.g + m.m[2][2] * v.b};
}

double Saturate(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

double SrgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double LinearToSrgb(double v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double Pq(double nits)
{
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    const double l = Saturate(nits / 10000.0);
    const double lm1 = std::pow(l, m1);
    return std::pow((c1 + c2 * lm1) / (1.0 + c3 * lm1), m2);
}

struct Params {
    bool hdr = false;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    double white_scale = 1.0;
    bool cursor = false;
    int cx = 0, cy = 0;
    const CursorImage* shape = nullptr;
};

void SourceTexel(const Params& p, int w, int h, int dx, int dy, int& sx, int& sy)
{
    switch (p.rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:  sx = dy;         sy = h - 1 - dx; break;
    case DXGI_MODE_ROTATION_ROTATE180: sx = w - 1 - dx; sy = h - 1 - dy; break;
    case DXGI_MODE_ROTATION_ROTATE270: sx = w - 1 - dy; sy = dx;         break;
    default:                           sx = dx;         sy = dy;         break;
    }
}

double XorHdr(double desktop, int code, double scale)
{
    if (code == 0) {
        return desktop;
    }
    const double e = LinearToSrgb(Saturate(desktop / scale));
    const int q = static_cast<int>(std::lround(e * 255.0));
    return SrgbToLinear((q ^ code) / 255.0) * scale;
}

Rgb DesktopWithCursor(const Params& p, const Image& src, int sx, int sy)
{
    Rgb v = src.At(sx, sy);
    if (!p.cursor) {
        return v;
    }
    const int cx = sx - p.cx;
    const int cy = sy - p.cy;
    if (cx < 0 || cy < 0 || cx >= p.shape->w || cy >= p.shape->h) {
        return v;
    }
    const uint8_t* t = &p.shape->bgra[(static_cast<size_t>(cy) * p.shape->w + cx) * 4];
    const int cb = t[0], cg = t[1], cr = t[2], ca = t[3];
    const Rgb enc{cr / 255.0, cg / 255.0, cb / 255.0};
    const Rgb lin{SrgbToLinear(enc.r) * p.white_scale, SrgbToLinear(enc.g) * p.white_scale,
                  SrgbToLinear(enc.b) * p.white_scale};
    const Rgb cur = p.hdr ? lin : enc;

    if (p.shape->kind == zcr::CursorKind::Color) {
        const double a = ca / 255.0;
        return {v.r + (cur.r - v.r) * a, v.g + (cur.g - v.g) * a, v.b + (cur.b - v.b) * a};
    }
    if (ca < 128) {
        return cur;
    }
    if (p.hdr) {
        return {XorHdr(v.r, cr, p.white_scale), XorHdr(v.g, cg, p.white_scale),
                XorHdr(v.b, cb, p.white_scale)};
    }
    auto x8 = [](double d, int c) {
        return (static_cast<int>(std::lround(Saturate(d) * 255.0)) ^ c) / 255.0;
    };
    return {x8(v.r, cr), x8(v.g, cg), x8(v.b, cb)};
}

Rgb NonLinear(const Params& p, const Image& src, int dx, int dy)
{
    int sx = 0, sy = 0;
    SourceTexel(p, src.w, src.h, dx, dy, sx, sy);
    const Rgb v = DesktopWithCursor(p, src, sx, sy);
    if (!p.hdr) {
        return {Saturate(v.r), Saturate(v.g), Saturate(v.b)};
    }
    const Rgb w = Apply(g_709_to_2020, v);
    return {Pq(std::max(w.r, 0.0) * 80.0), Pq(std::max(w.g, 0.0) * 80.0),
            Pq(std::max(w.b, 0.0) * 80.0)};
}

struct Weights {
    double kr, kb;
};

Weights WeightsFor(bool hdr)
{
    return hdr ? Weights{0.2627, 0.0593} : Weights{0.2126, 0.0722};
}

double Luma(const Weights& k, const Rgb& v)
{
    return k.kr * v.r + (1.0 - k.kr - k.kb) * v.g + k.kb * v.b;
}

// Unrounded codes, so the table can show how close to a boundary a value sits.
double YCode(double yp)
{
    return std::clamp(64.0 + 876.0 * yp, 64.0, 940.0);
}

double CCode(double c)
{
    return std::clamp(512.0 + 896.0 * c, 64.0, 960.0);
}

struct Planes {
    int w = 0, h = 0;
    std::vector<double> y;          // w*h
    std::vector<double> cb, cr;     // (w/2)*(h/2)
};

Planes Model(const Params& p, const Image& src)
{
    const bool swap = p.rotation == DXGI_MODE_ROTATION_ROTATE90
                      || p.rotation == DXGI_MODE_ROTATION_ROTATE270;
    Planes out;
    out.w = (swap ? src.h : src.w) & ~1;
    out.h = (swap ? src.w : src.h) & ~1;
    const Weights k = WeightsFor(p.hdr);
    out.y.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            out.y[static_cast<size_t>(y) * out.w + x] = YCode(Luma(k, NonLinear(p, src, x, y)));
        }
    }
    const int cw = out.w / 2, ch = out.h / 2;
    out.cb.resize(static_cast<size_t>(cw) * ch);
    out.cr.resize(out.cb.size());
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            Rgb s;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const Rgb v = NonLinear(p, src, 2 * x + i, 2 * y + j);
                    s.r += v.r / 4.0;
                    s.g += v.g / 4.0;
                    s.b += v.b / 4.0;
                }
            }
            const double yp = Luma(k, s);
            out.cb[static_cast<size_t>(y) * cw + x] = CCode((s.b - yp) / (2.0 * (1.0 - k.kb)));
            out.cr[static_cast<size_t>(y) * cw + x] = CCode((s.r - yp) / (2.0 * (1.0 - k.kr)));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Test content
// ---------------------------------------------------------------------------

struct Lcg {
    uint32_t state;
    double Next()
    {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / 16777216.0;
    }
};

struct Patch {
    const char* name;
    Rgb value;           // scRGB for HDR, encoded [0,1] for SDR
    int expect_y;        // hand-computed sanity value, -1 when none
    bool neutral;        // Cb and Cr must be 512
};

const Patch kHdrPatches[] = {
    {"0 nits", {0, 0, 0}, 64, true},
    {"80 nits (1.0)", {1, 1, 1}, 490, true},
    {"200 nits (2.5)", {2.5, 2.5, 2.5}, -1, true},
    {"1000 nits (12.5)", {12.5, 12.5, 12.5}, -1, true},
    {"10000 nits (125)", {125, 125, 125}, 940, true},
    {"16000 nits (200)", {200, 200, 200}, 940, true},
    {"709 red 80 nits", {1, 0, 0}, -1, false},
    {"709 green 80 nits", {0, 1, 0}, -1, false},
    {"709 blue 80 nits", {0, 0, 1}, -1, false},
    {"wide gamut (-.1,1.2,-.05)", {-0.1, 1.2, -0.05}, -1, false},
    {"outside 2020 (1,-.5,1)", {1, -0.5, 1}, -1, false},
    {"40 nits (0.5)", {0.5, 0.5, 0.5}, -1, true},
    {"240 nits (3.0)", {3, 3, 3}, -1, true},
    {"magenta 320 nits", {4, 0, 4}, -1, false},
    {"warm (2.5,1.25,.5)", {2.5, 1.25, 0.5}, -1, false},
    {"20 nits (0.25)", {0.25, 0.25, 0.25}, -1, true},
};

const Patch kSdrPatches[] = {
    {"black", {0, 0, 0}, 64, true},
    {"white", {1, 1, 1}, 940, true},
    {"gray 128", {128 / 255.0, 128 / 255.0, 128 / 255.0}, -1, true},
    {"red", {1, 0, 0}, -1, false},
    {"green", {0, 1, 0}, -1, false},
    {"blue", {0, 0, 1}, -1, false},
    {"gray 64", {64 / 255.0, 64 / 255.0, 64 / 255.0}, -1, true},
    {"orange", {1, 128 / 255.0, 0}, -1, false},
};

uint16_t ToHalf(double v)
{
    return DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(v));
}

double FromHalf(uint16_t h)
{
    return DirectX::PackedVector::XMConvertHalfToFloat(h);
}

// Patches in 4x4 blocks along the top rows (so each covers whole 2x2 chroma
// blocks in the identity case), random content everywhere else.
Image MakeSource(bool hdr, int w, int h, uint32_t seed, std::vector<uint8_t>& bytes)
{
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<size_t>(w) * h);
    Lcg rng{seed};
    for (auto& v : img.px) {
        if (hdr) {
            auto one = [&] {
                const double r = rng.Next();
                return -0.2 + r * r * r * 40.0;
            };
            v = {one(), one(), one()};
        } else {
            v = {std::floor(rng.Next() * 256.0) / 255.0, std::floor(rng.Next() * 256.0) / 255.0,
                 std::floor(rng.Next() * 256.0) / 255.0};
        }
    }
    const Patch* patches = hdr ? kHdrPatches : kSdrPatches;
    const int count = hdr ? static_cast<int>(std::size(kHdrPatches))
                          : static_cast<int>(std::size(kSdrPatches));
    for (int i = 0; i < count && (i + 1) * 4 <= w && h >= 4; ++i) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                img.px[static_cast<size_t>(y) * w + i * 4 + x] = patches[i].value;
            }
        }
    }

    // Upload bytes, and round the model's copy to exactly what the GPU sees.
    if (hdr) {
        bytes.resize(img.px.size() * 8);
        auto* out = reinterpret_cast<uint16_t*>(bytes.data());
        for (size_t i = 0; i < img.px.size(); ++i) {
            Rgb& v = img.px[i];
            out[i * 4 + 0] = ToHalf(v.r);
            out[i * 4 + 1] = ToHalf(v.g);
            out[i * 4 + 2] = ToHalf(v.b);
            out[i * 4 + 3] = ToHalf(1.0);
            v = {FromHalf(out[i * 4 + 0]), FromHalf(out[i * 4 + 1]), FromHalf(out[i * 4 + 2])};
        }
    } else {
        bytes.resize(img.px.size() * 4);
        for (size_t i = 0; i < img.px.size(); ++i) {
            Rgb& v = img.px[i];
            const auto code = [](double d) {
                return static_cast<uint8_t>(std::lround(Saturate(d) * 255.0));
            };
            bytes[i * 4 + 0] = code(v.b);
            bytes[i * 4 + 1] = code(v.g);
            bytes[i * 4 + 2] = code(v.r);
            bytes[i * 4 + 3] = 255;
            v = {bytes[i * 4 + 2] / 255.0, bytes[i * 4 + 1] / 255.0, bytes[i * 4 + 0] / 255.0};
        }
    }
    return img;
}

CursorImage MakeColorCursor()
{
    CursorImage c;
    c.kind = zcr::CursorKind::Color;
    c.w = 6;
    c.h = 5;
    // B, G, R, A per texel. Straight alpha: opaque, half, quarter, clear.
    const uint8_t texels[][4] = {
        {255, 255, 255, 255}, {0, 0, 0, 255},     {0, 0, 255, 128},   {255, 0, 0, 64},
        {0, 255, 0, 0},       {200, 100, 50, 200},
    };
    c.bgra.resize(static_cast<size_t>(c.w) * c.h * 4);
    for (int i = 0; i < c.w * c.h; ++i) {
        const auto& t = texels[(i * 7 + i / c.w) % std::size(texels)];
        std::copy(t, t + 4, &c.bgra[static_cast<size_t>(i) * 4]);
    }
    return c;
}

CursorImage MakeMaskedCursor()
{
    CursorImage c;
    c.kind = zcr::CursorKind::Masked;
    c.w = 6;
    c.h = 5;
    const uint8_t texels[][4] = {
        {255, 255, 255, 255}, // invert
        {0, 0, 0, 255},       // transparent
        {0, 0, 0, 0},         // opaque black
        {255, 255, 255, 0},   // opaque white
        {0x33, 0xAA, 0x55, 255},
        {0, 0, 255, 0},       // opaque red
    };
    c.bgra.resize(static_cast<size_t>(c.w) * c.h * 4);
    for (int i = 0; i < c.w * c.h; ++i) {
        const auto& t = texels[(i * 5 + i / c.w) % std::size(texels)];
        std::copy(t, t + 4, &c.bgra[static_cast<size_t>(i) * 4]);
    }
    return c;
}

// ---------------------------------------------------------------------------
// GPU side
// ---------------------------------------------------------------------------

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> info;
    std::wstring adapter;
};

bool CreateNvidiaDevice(Gpu& gpu)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) {
            gpu.adapter = desc.Description;
            break;
        }
        adapter.Reset();
    }
    if (!adapter) {
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // The debug layer reports binding hazards and bad views; use it when the
    // SDK layers are installed, run without it when they are not.
    for (UINT flags : {UINT{D3D11_CREATE_DEVICE_DEBUG}, UINT{0}}) {
        const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             flags | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                             static_cast<UINT>(std::size(levels)),
                                             D3D11_SDK_VERSION, &gpu.device, nullptr,
                                             &gpu.context);
        if (SUCCEEDED(hr)) {
            gpu.device.As(&gpu.info);
            return true;
        }
    }
    return false;
}

// Prints and counts debug-layer warnings and errors since the last call.
int DrainDebugMessages(Gpu& gpu)
{
    if (!gpu.info) {
        return 0;
    }
    int bad = 0;
    const UINT64 n = gpu.info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T size = 0;
        gpu.info->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
        if (FAILED(gpu.info->GetMessage(i, msg, &size))) {
            continue;
        }
        if (msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            std::printf("    d3d11 debug: %.*s\n", static_cast<int>(msg->DescriptionByteLength),
                        msg->pDescription);
            ++bad;
        }
    }
    gpu.info->ClearStoredMessages();
    return bad;
}

ComPtr<ID3D11ShaderResourceView> MakeTexture(Gpu& gpu, DXGI_FORMAT format, int w, int h,
                                             const void* data, UINT bpp)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, static_cast<UINT>(w) * bpp, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(gpu.device->CreateTexture2D(&desc, &init, &tex))
        || FAILED(gpu.device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        return nullptr;
    }
    return srv;
}

ComPtr<ID3D11Texture2D> MakeP010(Gpu& gpu, int w, int h, bool staging)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_P010;
    desc.SampleDesc.Count = 1;
    desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.BindFlags = staging ? 0u : static_cast<UINT>(D3D11_BIND_RENDER_TARGET);
    desc.CPUAccessFlags = staging ? static_cast<UINT>(D3D11_CPU_ACCESS_READ) : 0u;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &tex))) {
        return nullptr;
    }
    return tex;
}

void ProbeRtvMethods(Gpu& gpu)
{
    std::printf("RTV creation on a P010 texture:\n");
    ComPtr<ID3D11Texture2D> tex = MakeP010(gpu, 64, 64, false);
    if (!tex) {
        std::printf("  cannot create a P010 render target at all\n");
        return;
    }
    for (auto [format, plane, name] :
         {std::tuple{DXGI_FORMAT_R16_UNORM, 0u, "R16_UNORM (luma)"},
          std::tuple{DXGI_FORMAT_R16G16_UNORM, 1u, "R16G16_UNORM (chroma)"}}) {
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        const HRESULT plain = gpu.device->CreateRenderTargetView(tex.Get(), &d, &rtv);

        HRESULT v1 = E_NOINTERFACE;
        ComPtr<ID3D11Device3> d3;
        if (SUCCEEDED(gpu.device.As(&d3))) {
            D3D11_RENDER_TARGET_VIEW_DESC1 d1{};
            d1.Format = format;
            d1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            d1.Texture2D.PlaneSlice = plane;
            ComPtr<ID3D11RenderTargetView1> rtv1;
            v1 = d3->CreateRenderTargetView1(tex.Get(), &d1, &rtv1);
        }
        std::printf("  %-22s CreateRenderTargetView 0x%08lX   CreateRenderTargetView1(PlaneSlice %u) 0x%08lX\n",
                    name, static_cast<unsigned long>(plain), plane, static_cast<unsigned long>(v1));
    }
    DrainDebugMessages(gpu);
}

struct Readback {
    int w = 0, h = 0;
    std::vector<uint16_t> y, cb, cr;   // raw 16-bit words
};

bool ReadP010(Gpu& gpu, ID3D11Texture2D* target, int w, int h, Readback& out)
{
    ComPtr<ID3D11Texture2D> staging = MakeP010(gpu, w, h, true);
    if (!staging) {
        return false;
    }
    gpu.context->CopyResource(staging.Get(), target);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return false;
    }
    out.w = w;
    out.h = h;
    out.y.resize(static_cast<size_t>(w) * h);
    out.cb.resize(static_cast<size_t>(w / 2) * (h / 2));
    out.cr.resize(out.cb.size());
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * m.RowPitch);
        std::copy(row, row + w, &out.y[static_cast<size_t>(y) * w]);
    }
    // The CbCr plane follows the luma plane at the same pitch.
    const uint8_t* uv = base + static_cast<size_t>(m.RowPitch) * h;
    for (int y = 0; y < h / 2; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(uv + static_cast<size_t>(y) * m.RowPitch);
        for (int x = 0; x < w / 2; ++x) {
            out.cb[static_cast<size_t>(y) * (w / 2) + x] = row[x * 2 + 0];
            out.cr[static_cast<size_t>(y) * (w / 2) + x] = row[x * 2 + 1];
        }
    }
    gpu.context->Unmap(staging.Get(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

struct Case {
    const char* name;
    bool hdr;
    DXGI_MODE_ROTATION rotation;
    int w, h;
    int cursor;             // 0 none, 1 color, 2 masked
    int cx, cy;
    float sdr_white_nits;
    bool draw_cursor;
    bool spot_table;
};

const char* RotationName(DXGI_MODE_ROTATION r)
{
    switch (r) {
    case DXGI_MODE_ROTATION_ROTATE90:  return "90";
    case DXGI_MODE_ROTATION_ROTATE180: return "180";
    case DXGI_MODE_ROTATION_ROTATE270: return "270";
    default:                           return "0";
    }
}

void PrintSpotTable(const Case& c, const Planes& model, const Readback& gpu)
{
    const Patch* patches = c.hdr ? kHdrPatches : kSdrPatches;
    const int count = c.hdr ? static_cast<int>(std::size(kHdrPatches))
                            : static_cast<int>(std::size(kSdrPatches));
    std::printf("    %-27s %10s %10s %10s   %5s %5s\n", "patch", "Y gpu/cpu", "Cb gpu/cpu",
                "Cr gpu/cpu", "hand", "check");
    for (int i = 0; i < count && (i + 1) * 4 <= model.w; ++i) {
        const size_t yi = static_cast<size_t>(i) * 4;
        const size_t ci = static_cast<size_t>(i) * 2;
        const int y = gpu.y[yi] >> 6, cb = gpu.cb[ci] >> 6, cr = gpu.cr[ci] >> 6;
        bool ok = true;
        if (patches[i].expect_y >= 0) {
            ok = ok && std::abs(y - patches[i].expect_y) <= 1;
        }
        if (patches[i].neutral) {
            ok = ok && cb == 512 && cr == 512;
        }
        char hand[16] = "-";
        if (patches[i].expect_y >= 0) {
            std::snprintf(hand, sizeof(hand), "%d", patches[i].expect_y);
        }
        std::printf("    %-27s %4d/%6.1f %4d/%6.1f %4d/%6.1f   %5s %5s\n", patches[i].name, y,
                    model.y[yi], cb, model.cb[ci], cr, model.cr[ci], hand, ok ? "ok" : "FAIL");
    }
}

bool RunCase(Gpu& gpu, zcr::Converter& converter, const Case& c, uint32_t seed)
{
    std::vector<uint8_t> bytes;
    const Image src = MakeSource(c.hdr, c.w, c.h, seed, bytes);
    ComPtr<ID3D11ShaderResourceView> src_srv =
        MakeTexture(gpu, c.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM, c.w,
                    c.h, bytes.data(), c.hdr ? 8u : 4u);

    const CursorImage shape = c.cursor == 2 ? MakeMaskedCursor() : MakeColorCursor();
    zcr::CursorState cursor;
    if (c.cursor != 0) {
        cursor.visible = true;
        cursor.x = c.cx;
        cursor.y = c.cy;
        cursor.width = shape.w;
        cursor.height = shape.h;
        cursor.kind = shape.kind;
        cursor.shape = MakeTexture(gpu, DXGI_FORMAT_B8G8R8A8_UNORM, shape.w, shape.h,
                                   shape.bgra.data(), 4);
    }

    Params p;
    p.hdr = c.hdr;
    p.rotation = c.rotation;
    p.white_scale = static_cast<double>(c.sdr_white_nits) / 80.0;
    p.cursor = c.cursor != 0 && c.draw_cursor;
    p.cx = c.cx;
    p.cy = c.cy;
    p.shape = &shape;
    const Planes model = Model(p, src);

    ComPtr<ID3D11Texture2D> target = MakeP010(gpu, model.w, model.h, false);
    if (!src_srv || !target) {
        std::printf("FAIL  %s: cannot create textures\n", c.name);
        return false;
    }

    zcr::ConvertParams params;
    params.hdr = c.hdr;
    params.sdr_white_nits = c.sdr_white_nits;
    params.rotation = c.rotation;
    params.draw_cursor = c.draw_cursor;
    std::wstring error;
    if (!converter.Convert(src_srv.Get(), static_cast<UINT>(c.w), static_cast<UINT>(c.h), cursor,
                           target.Get(), params, error)) {
        std::printf("FAIL  %s: Convert: %ls\n", c.name, error.c_str());
        return false;
    }

    Readback rb;
    if (!ReadP010(gpu, target.Get(), model.w, model.h, rb)) {
        std::printf("FAIL  %s: readback\n", c.name);
        return false;
    }

    double max_y = 0, max_c = 0;
    long bad = 0, low_bits = 0;
    auto check = [&](uint16_t word, double expect, double& worst) {
        if (word & 0x3F) {
            ++low_bits;
        }
        const double d = std::abs((word >> 6) - expect);
        worst = std::max(worst, d);
        if (d > 1.0) {
            ++bad;
        }
    };
    for (size_t i = 0; i < rb.y.size(); ++i) {
        check(rb.y[i], model.y[i], max_y);
    }
    for (size_t i = 0; i < rb.cb.size(); ++i) {
        check(rb.cb[i], model.cb[i], max_c);
        check(rb.cr[i], model.cr[i], max_c);
    }
    const int debug = DrainDebugMessages(gpu);
    const bool ok = bad == 0 && low_bits == 0 && debug == 0;

    char title[160];
    std::snprintf(title, sizeof(title), "%s  [%s %dx%d rot %s -> %dx%d, white %.0f]", c.name,
                  c.hdr ? "HDR" : "SDR", c.w, c.h, RotationName(c.rotation), model.w, model.h,
                  static_cast<double>(c.sdr_white_nits));
    std::printf("%s  %-78s max|dY| %.2f  max|dC| %.2f  >1: %ld  low6!=0: %ld\n", ok ? "pass" : "FAIL",
                title, max_y, max_c, bad, low_bits);
    if (c.spot_table) {
        PrintSpotTable(c, model, rb);
    }
    return ok;
}

} // namespace
} // namespace zcr::test

int main()
{
    using namespace zcr::test;

    g_709_to_2020 = Multiply(Inverse(RgbToXyz(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290)),
                             RgbToXyz(0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290));
    const double shader_matrix[3][3] = {
        {0.627403895934699, 0.329283038377884, 0.043313065687417},
        {0.069097289358232, 0.919540395075459, 0.011362315566309},
        {0.016391438875150, 0.088013307877226, 0.895595253247624},
    };
    double matrix_err = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            matrix_err = std::max(matrix_err, std::abs(shader_matrix[i][j] - g_709_to_2020.m[i][j]));
        }
    }
    std::printf("BT.709->BT.2020 matrix: shader constants vs derived from primaries, max |diff| %.2e  %s\n",
                matrix_err, matrix_err < 1e-9 ? "ok" : "FAIL");

    Gpu gpu;
    if (!CreateNvidiaDevice(gpu)) {
        std::printf("FAIL: no NVIDIA D3D11 device\n");
        return 2;
    }
    std::printf("Adapter: %ls%s\n\n", gpu.adapter.c_str(), gpu.info ? " (debug layer on)" : "");
    ProbeRtvMethods(gpu);
    std::printf("\n");

    zcr::Converter converter;
    std::wstring error;
    if (!converter.Init(gpu.device.Get(), error)) {
        std::printf("FAIL: Init: %ls\n", error.c_str());
        return 2;
    }

    const auto R0 = DXGI_MODE_ROTATION_IDENTITY;
    const auto R90 = DXGI_MODE_ROTATION_ROTATE90;
    const auto R180 = DXGI_MODE_ROTATION_ROTATE180;
    const auto R270 = DXGI_MODE_ROTATION_ROTATE270;
    const Case cases[] = {
        {"HDR patches", true, R0, 64, 8, 0, 0, 0, 80.0f, true, true},
        {"HDR color cursor", true, R0, 64, 8, 1, 10, 2, 200.0f, true, false},
        {"HDR masked cursor over 1000-nit patch", true, R0, 64, 8, 2, 11, 1, 240.0f, true, false},
        {"HDR masked cursor, 80-nit white", true, R0, 64, 8, 2, 3, 3, 80.0f, true, false},
        {"HDR cursor clipped top-left", true, R0, 64, 8, 1, -3, -2, 200.0f, true, false},
        {"HDR cursor clipped bottom-right", true, R0, 64, 8, 2, 61, 5, 200.0f, true, false},
        {"HDR cursor present, draw_cursor off", true, R0, 64, 8, 1, 10, 2, 200.0f, false, false},
        {"HDR rotate 90", true, R90, 20, 12, 1, 4, 3, 200.0f, true, false},
        {"HDR rotate 180", true, R180, 20, 12, 2, 12, 6, 200.0f, true, false},
        {"HDR rotate 270", true, R270, 20, 12, 1, 15, 8, 200.0f, true, false},
        {"HDR rotate 90, odd size", true, R90, 21, 13, 2, 17, 9, 160.0f, true, false},
        {"HDR rotate 270, odd size", true, R270, 21, 13, 1, 0, 0, 160.0f, true, false},
        {"HDR identity, odd size", true, R0, 21, 13, 2, 18, 10, 160.0f, true, false},
        {"HDR 3840x2160 rotate 90", true, R90, 3840, 2160, 2, 1000, 700, 203.0f, true, false},
        {"SDR patches", false, R0, 64, 8, 0, 0, 0, 80.0f, true, true},
        {"SDR color cursor", false, R0, 64, 8, 1, 6, 1, 80.0f, true, false},
        {"SDR masked cursor", false, R0, 64, 8, 2, 2, 2, 80.0f, true, false},
        {"SDR rotate 90", false, R90, 20, 12, 2, 4, 3, 80.0f, true, false},
        {"SDR rotate 180, odd size", false, R180, 21, 13, 1, 9, 5, 80.0f, true, false},
        {"SDR rotate 270", false, R270, 20, 12, 2, 14, 7, 80.0f, true, false},
        {"SDR 2560x1440 identity", false, R0, 2560, 1440, 1, 2556, 1437, 80.0f, true, false},
    };

    int failed = matrix_err < 1e-9 ? 0 : 1;
    uint32_t seed = 12345;
    for (const Case& c : cases) {
        if (!RunCase(gpu, converter, c, seed++)) {
            ++failed;
        }
        // Each case uses a fresh target; drop the cached views so the test
        // also exercises view creation every time.
        converter.ForgetTargets();
    }
    converter.Shutdown();

    std::printf("\n%s: %d of %zu checks failed\n", failed ? "FAIL" : "PASS", failed,
                std::size(cases) + 1);
    return failed ? 1 : 0;
}
