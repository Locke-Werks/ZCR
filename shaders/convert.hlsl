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

// Desktop surface -> P010, one pass per plane, or -> R10G10B10A2 in one pass
// for 4:4:4. See src/convert.h for the
// contract and src/convert.cpp for the constant buffer layout, which must match
// ConvertConstants below byte for byte.
//
// Everything reads the source with Load() at integer texel coordinates. There
// is no sampler anywhere: the output grid maps one to one onto source texels
// (rotation only permutes them), so filtering could only blur.

cbuffer ConvertConstants : register(b0)
{
    int2  g_src_size;      // source surface, unrotated
    int2  g_dst_size;      // luma plane, display orientation, even
    int2  g_cursor_pos;    // top-left of the shape, source coordinates
    int2  g_cursor_size;
    uint  g_rotation;      // 0 identity, 1 = 90, 2 = 180, 3 = 270
    uint  g_hdr;           // 1: FP16 scRGB source, 0: BGRA8 sRGB-encoded
    uint  g_draw_cursor;
    uint  g_cursor_kind;   // 0 Color, 1 Masked (CursorKind order)
    float g_white_scale;   // sdr_white_nits / 80: cursor white in scRGB units
    float3 g_pad;
};

Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_cursor : register(t1);

// ITU-R BT.2087 linear BT.709 -> BT.2020, derived from the two sets of xy
// primaries with a shared D65 white. Rows sum to 1 so neutrals stay neutral.
static const float3x3 kBt709ToBt2020 = {
    0.627403895934699, 0.329283038377884, 0.043313065687417,
    0.069097289358232, 0.919540395075459, 0.011362315566309,
    0.016391438875150, 0.088013307877226, 0.895595253247624,
};

// SMPTE ST 2084.
static const float kPqM1 = 2610.0 / 16384.0;
static const float kPqM2 = 2523.0 / 4096.0 * 128.0;
static const float kPqC1 = 3424.0 / 4096.0;
static const float kPqC2 = 2413.0 / 4096.0 * 32.0;
static const float kPqC3 = 2392.0 / 4096.0 * 32.0;

// scRGB defines 1.0 as 80 nits; PQ is normalized to 10000 nits.
static const float kScrgbToPq = 80.0 / 10000.0;

struct VSOut {
    float4 pos : SV_Position;
};

// One triangle covering the viewport, positions from SV_VertexID, so there is
// no vertex buffer and no input layout to keep in sync with anything.
VSOut VSMain(uint id : SV_VertexID)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Display-orientation pixel -> source texel. The source is the panel's native
// scanout orientation; ROTATE90 means the image must be turned 90 degrees
// clockwise to be upright, so the source's top-left lands at the target's
// top-right. Same mapping as the Desktop Duplication sample and OBS.
int2 SourceTexel(int2 d)
{
    const int w = g_src_size.x;
    const int h = g_src_size.y;
    switch (g_rotation) {
    case 1:  return int2(d.y, h - 1 - d.x);
    case 2:  return int2(w - 1 - d.x, h - 1 - d.y);
    case 3:  return int2(w - 1 - d.y, d.x);
    default: return d;
    }
}

float SrgbToLinear(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow(abs((v + 0.055) / 1.055), 2.4);
}

float3 SrgbToLinear(float3 v)
{
    return float3(SrgbToLinear(v.r), SrgbToLinear(v.g), SrgbToLinear(v.b));
}

float LinearToSrgb(float v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(abs(v), 1.0 / 2.4) - 0.055;
}

// 8-bit codes, exact for UNORM8 inputs, where XOR has to happen.
uint3 ToCode8(float3 v)
{
    return (uint3)round(saturate(v) * 255.0);
}

// XOR one channel of the desktop with a Masked cursor texel. The XOR is done on
// 8-bit sRGB codes because that is the domain Windows defines it in: an
// I-beam's white XOR turns black on white paper and white on black, which only
// holds if "white" means SDR white. In HDR the desktop is first brought back to
// SDR-relative sRGB (divide by the SDR white scale), quantized, XORed and
// returned to scRGB. A zero cursor channel is the identity, so it is skipped
// rather than quantized: transparent texels in the shape's box must leave HDR
// highlights above SDR white untouched.
float XorChannelHdr(float desktop, uint code)
{
    if (code == 0) {
        return desktop;
    }
    const float encoded = LinearToSrgb(saturate(desktop / g_white_scale));
    const uint q = (uint)round(encoded * 255.0);
    return SrgbToLinear((float)(q ^ code) / 255.0) * g_white_scale;
}

// Desktop texel with the cursor composited, in the source's own domain: linear
// scRGB for HDR, sRGB-encoded [0,1] for SDR.
float3 DesktopWithCursor(int2 s)
{
    float3 rgb = g_source.Load(int3(s, 0)).rgb;
    if (g_draw_cursor == 0) {
        return rgb;
    }
    const int2 c = s - g_cursor_pos;
    if (any(c < 0) || any(c >= g_cursor_size)) {
        return rgb;
    }

    const float4 texel = g_cursor.Load(int3(c, 0));
    if (g_cursor_kind == 0) {
        // Color: straight alpha. HDR blends in linear light with the cursor at
        // SDR white so it matches what the user sees; SDR blends the encoded
        // values the way DWM composites an SDR desktop.
        const float3 cur = g_hdr != 0 ? SrgbToLinear(texel.rgb) * g_white_scale : texel.rgb;
        return lerp(rgb, cur, texel.a);
    }

    // Masked: alpha is exactly 0 or 1 by construction in capture.cpp; the
    // midpoint test keeps UNORM rounding from mattering.
    if (texel.a < 0.5) {
        return g_hdr != 0 ? SrgbToLinear(texel.rgb) * g_white_scale : texel.rgb;
    }
    const uint3 code = ToCode8(texel.rgb);
    if (g_hdr != 0) {
        return float3(XorChannelHdr(rgb.r, code.r),
                      XorChannelHdr(rgb.g, code.g),
                      XorChannelHdr(rgb.b, code.b));
    }
    return (float3)(ToCode8(rgb) ^ code) / 255.0;
}

float3 PqEncode(float3 nits_over_10000)
{
    const float3 l = saturate(nits_over_10000);
    const float3 lm1 = pow(l, kPqM1);
    return pow(saturate((kPqC1 + kPqC2 * lm1) / (1.0 + kPqC3 * lm1)), kPqM2);
}

// Non-linear R'G'B' in the output's primaries, ready for the Y'CbCr matrix:
// PQ BT.2020 for HDR, sRGB-encoded BT.709 as-is for SDR.
float3 NonLinearAt(int2 d)
{
    const float3 rgb = DesktopWithCursor(SourceTexel(d));
    if (g_hdr == 0) {
        return saturate(rgb);
    }
    // Negative scRGB is how wide-gamut color is expressed; after the move to
    // BT.2020 anything still negative is outside BT.2020 as well and is
    // clipped. The max also turns NaN into 0 on this hardware.
    const float3 rgb2020 = max(mul(kBt709ToBt2020, rgb), 0.0);
    return PqEncode(rgb2020 * kScrgbToPq);
}

// Luma and chroma weights. BT.2020 non-constant luminance for HDR, BT.709 for
// SDR, matching the VUI written by nvenc.cpp (matrix 9 or 1).
float3 LumaWeights()
{
    return g_hdr != 0 ? float3(0.2627, 0.6780, 0.0593) : float3(0.2126, 0.7152, 0.0722);
}

// 10-bit code -> R16_UNORM value. P010 keeps the code in the top 10 bits of the
// 16-bit word, so the word is code << 6 and the UNORM value is that over 65535.
// code / 1023 would put 1023 at 65535 and smear the low bits into every word.
float CodeToP010(float code)
{
    return code * 64.0 / 65535.0;
}

// Codes are clamped to the nominal limited range (Y 64..940, C 64..960) rather
// than the 4..1019 BT.2100 tolerates. With PQ and R'G'B' already clamped to
// [0,1] the matrix cannot leave that range anyway; the clamp only guards
// against rounding at the extremes.
float LumaCode(float yp)
{
    return clamp(round(64.0 + 876.0 * yp), 64.0, 940.0);
}

float ChromaCode(float c)
{
    return clamp(round(512.0 + 896.0 * c), 64.0, 960.0);
}

float PSLuma(VSOut i) : SV_Target
{
    const float3 rgb = NonLinearAt(int2(i.pos.xy));
    return CodeToP010(LumaCode(dot(LumaWeights(), rgb)));
}

// 4:4:4: the same R'G'B' the luma and chroma passes feed their matrix, written
// to an R10G10B10A2_UNORM target that NVENC converts with the matrix and range
// the VUI declares (src/nvenc.cpp). Quantized here rather than by the output
// merger so every code is the nearest one by construction.
float4 PSRgb(VSOut i) : SV_Target
{
    const float3 rgb = NonLinearAt(int2(i.pos.xy));
    return float4(round(rgb * 1023.0) / 1023.0, 1.0);
}

// Half resolution. Each chroma sample is the box average of its 2x2 luma
// block's non-linear R'G'B', then the matrix. That places the sample at the
// block center rather than the left (MPEG-2 style) siting the bitstream
// signals by default; the quarter-pixel horizontal offset is not visible on
// desktop content and the box filter needs no neighbors outside the block.
float2 PSChroma(VSOut i) : SV_Target
{
    const int2 d = int2(i.pos.xy) * 2;
    const float3 rgb = (NonLinearAt(d) + NonLinearAt(d + int2(1, 0)) +
                        NonLinearAt(d + int2(0, 1)) + NonLinearAt(d + int2(1, 1))) * 0.25;

    const float3 k = LumaWeights();
    const float yp = dot(k, rgb);
    const float cb = (rgb.b - yp) / (2.0 * (1.0 - k.b));
    const float cr = (rgb.r - yp) / (2.0 * (1.0 - k.r));
    return float2(CodeToP010(ChromaCode(cb)), CodeToP010(ChromaCode(cr)));
}
