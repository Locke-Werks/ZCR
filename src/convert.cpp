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
#include "convert.h"

#include <d3d11_3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

#include "shaders/convert_ps_chroma.h"
#include "shaders/convert_ps_luma.h"
#include "shaders/convert_ps_rgb.h"
#include "shaders/convert_vs.h"

namespace zcr {

using Microsoft::WRL::ComPtr;

namespace {

// Mirrors cbuffer ConvertConstants in shaders/convert.hlsl. HLSL packs into
// 16-byte registers and nothing here straddles one, so plain C layout matches.
struct ConvertConstants {
    int32_t src_size[2];
    int32_t dst_size[2];
    int32_t cursor_pos[2];
    int32_t cursor_size[2];
    uint32_t rotation;      // 0 identity, 1 = 90, 2 = 180, 3 = 270
    uint32_t hdr;
    uint32_t draw_cursor;
    uint32_t cursor_kind;   // 0 Color, 1 Masked
    float white_scale;      // sdr_white_nits / 80
    float pad[3];
};
static_assert(sizeof(ConvertConstants) == 64, "must match the HLSL cbuffer");
static_assert(sizeof(ConvertConstants) % 16 == 0, "constant buffers are sized in 16-byte units");

std::wstring HrText(const wchar_t* what, HRESULT hr)
{
    wchar_t buffer[160];
    std::swprintf(buffer, std::size(buffer), L"%ls failed: 0x%08lX", what,
                  static_cast<unsigned long>(hr));
    return buffer;
}

[[nodiscard]] uint32_t RotationIndex(DXGI_MODE_ROTATION rotation)
{
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:  return 1;
    case DXGI_MODE_ROTATION_ROTATE180: return 2;
    case DXGI_MODE_ROTATION_ROTATE270: return 3;
    default:                           return 0; // IDENTITY, and UNSPECIFIED means none
    }
}

[[nodiscard]] bool SwapsAxes(DXGI_MODE_ROTATION rotation)
{
    return rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
}

// Planar RTVs on the two planes of a P010 texture. D3D11.1 selects the plane
// from the view format (R16 is luma, R16G16 is the interleaved CbCr plane),
// which is what the plain call relies on. D3D11.3 added an explicit PlaneSlice;
// it is the fallback in case a driver only honors that form.
[[nodiscard]] HRESULT CreatePlaneView(ID3D11Device* device, ID3D11Texture2D* texture,
                                      DXGI_FORMAT format, UINT plane,
                                      ComPtr<ID3D11RenderTargetView>& out)
{
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    HRESULT hr = device->CreateRenderTargetView(texture, &desc, out.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        return hr;
    }

    ComPtr<ID3D11Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
        return hr;
    }
    D3D11_RENDER_TARGET_VIEW_DESC1 desc1{};
    desc1.Format = format;
    desc1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc1.Texture2D.MipSlice = 0;
    desc1.Texture2D.PlaneSlice = plane;
    ComPtr<ID3D11RenderTargetView1> view1;
    const HRESULT hr1 = device3->CreateRenderTargetView1(texture, &desc1, &view1);
    if (FAILED(hr1)) {
        return hr;
    }
    out = view1;
    return hr1;
}

} // namespace

struct Converter::Pipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> luma;
    ComPtr<ID3D11PixelShader> chroma;
    ComPtr<ID3D11PixelShader> rgb;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
};

Converter::Converter() = default;
Converter::~Converter() = default;

bool Converter::Init(ID3D11Device* device, std::wstring& error)
{
    Shutdown();
    if (!device) {
        error = L"Converter::Init: no device";
        return false;
    }

    auto p = std::make_unique<Pipeline>();
    HRESULT hr = device->CreateVertexShader(g_VSMain, sizeof(g_VSMain), nullptr, &p->vs);
    if (FAILED(hr)) {
        error = HrText(L"CreateVertexShader(VSMain)", hr);
        return false;
    }
    hr = device->CreatePixelShader(g_PSLuma, sizeof(g_PSLuma), nullptr, &p->luma);
    if (FAILED(hr)) {
        error = HrText(L"CreatePixelShader(PSLuma)", hr);
        return false;
    }
    hr = device->CreatePixelShader(g_PSChroma, sizeof(g_PSChroma), nullptr, &p->chroma);
    if (FAILED(hr)) {
        error = HrText(L"CreatePixelShader(PSChroma)", hr);
        return false;
    }
    hr = device->CreatePixelShader(g_PSRgb, sizeof(g_PSRgb), nullptr, &p->rgb);
    if (FAILED(hr)) {
        error = HrText(L"CreatePixelShader(PSRgb)", hr);
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ConvertConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cb, nullptr, &p->constants);
    if (FAILED(hr)) {
        error = HrText(L"CreateBuffer(constants)", hr);
        return false;
    }

    // The default state culls back faces; the fullscreen triangle is wound
    // front-facing, but not culling at all removes the one way a winding slip
    // could silently produce an empty frame.
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rs, &p->raster);
    if (FAILED(hr)) {
        error = HrText(L"CreateRasterizerState", hr);
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);
    pipeline_ = std::move(p);
    return true;
}

void Converter::Shutdown()
{
    targets_.clear();
    pipeline_.reset();
    context_.Reset();
    device_.Reset();
}

void Converter::ForgetTargets()
{
    // The cached views hold references to their textures, which is also what
    // makes keying by raw pointer safe: a texture cannot be freed, and its
    // address reused, while its entry is here.
    targets_.clear();
}

bool Converter::Convert(ID3D11ShaderResourceView* source, UINT source_width, UINT source_height,
                        const CursorState& cursor, ID3D11Texture2D* target,
                        const ConvertParams& params, std::wstring& error)
{
    Pipeline* p = pipeline_.get();
    if (!p || !device_ || !context_) {
        error = L"Converter::Convert called before Init";
        return false;
    }
    if (!source || !target) {
        error = L"Converter::Convert: null source or target";
        return false;
    }

    const bool swap = SwapsAxes(params.rotation);
    const UINT out_width = (swap ? source_height : source_width) & ~1u;
    const UINT out_height = (swap ? source_width : source_height) & ~1u;
    if (out_width < 2 || out_height < 2) {
        error = L"Converter::Convert: source too small";
        return false;
    }

    auto it = targets_.find(target);
    if (it == targets_.end()) {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        const bool rgb = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
        if ((desc.Format != DXGI_FORMAT_P010 && !rgb)
            || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
            error = L"Converter::Convert: target is not a P010 or R10G10B10A2 render target";
            return false;
        }
        TargetViews views;
        views.width = desc.Width;
        views.height = desc.Height;
        if (rgb) {
            const HRESULT hr = device_->CreateRenderTargetView(target, nullptr, &views.rgb);
            if (FAILED(hr)) {
                error = HrText(L"CreateRenderTargetView(R10G10B10A2_UNORM)", hr);
                return false;
            }
        } else {
            HRESULT hr =
                CreatePlaneView(device_.Get(), target, DXGI_FORMAT_R16_UNORM, 0, views.luma);
            if (FAILED(hr)) {
                error = HrText(L"CreateRenderTargetView(P010 luma, R16_UNORM)", hr);
                return false;
            }
            hr = CreatePlaneView(device_.Get(), target, DXGI_FORMAT_R16G16_UNORM, 1, views.chroma);
            if (FAILED(hr)) {
                error = HrText(L"CreateRenderTargetView(P010 chroma, R16G16_UNORM)", hr);
                return false;
            }
        }
        it = targets_.emplace(target, std::move(views)).first;
    }
    const TargetViews& views = it->second;
    if (views.width != out_width || views.height != out_height) {
        error = L"Converter::Convert: target is " + std::to_wstring(views.width) + L"x"
                + std::to_wstring(views.height) + L", expected " + std::to_wstring(out_width)
                + L"x" + std::to_wstring(out_height);
        return false;
    }

    const bool with_cursor = params.draw_cursor && cursor.visible && cursor.shape
                             && cursor.width > 0 && cursor.height > 0;

    ConvertConstants k{};
    k.src_size[0] = static_cast<int32_t>(source_width);
    k.src_size[1] = static_cast<int32_t>(source_height);
    k.dst_size[0] = static_cast<int32_t>(out_width);
    k.dst_size[1] = static_cast<int32_t>(out_height);
    k.cursor_pos[0] = cursor.x;
    k.cursor_pos[1] = cursor.y;
    k.cursor_size[0] = cursor.width;
    k.cursor_size[1] = cursor.height;
    k.rotation = RotationIndex(params.rotation);
    k.hdr = params.hdr ? 1u : 0u;
    k.draw_cursor = with_cursor ? 1u : 0u;
    k.cursor_kind = cursor.kind == CursorKind::Masked ? 1u : 0u;
    // A zero or negative white level would divide by zero in the HDR XOR path;
    // fall back to the scRGB reference white rather than emit NaN.
    k.white_scale = params.sdr_white_nits > 0.0f ? params.sdr_white_nits / 80.0f : 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context_->Map(p->constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error = HrText(L"Map(constants)", hr);
        return false;
    }
    std::memcpy(mapped.pData, &k, sizeof(k));
    context_->Unmap(p->constants.Get(), 0);

    ID3D11DeviceContext* ctx = context_.Get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p->vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(p->raster.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(nullptr, 0);

    ID3D11Buffer* cbs[] = {p->constants.Get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = {source, with_cursor ? cursor.shape.Get() : nullptr};
    ctx->PSSetShaderResources(0, 2, srvs);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(out_width);
    vp.Height = static_cast<float>(out_height);
    vp.MaxDepth = 1.0f;
    if (views.rgb) {
        ID3D11RenderTargetView* rgb = views.rgb.Get();
        ctx->OMSetRenderTargets(1, &rgb, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(p->rgb.Get(), nullptr, 0);
        ctx->Draw(3, 0);
    } else {
        ID3D11RenderTargetView* luma = views.luma.Get();
        ctx->OMSetRenderTargets(1, &luma, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(p->luma.Get(), nullptr, 0);
        ctx->Draw(3, 0);

        vp.Width = static_cast<float>(out_width / 2);
        vp.Height = static_cast<float>(out_height / 2);
        ID3D11RenderTargetView* chroma = views.chroma.Get();
        ctx->OMSetRenderTargets(1, &chroma, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(p->chroma.Get(), nullptr, 0);
        ctx->Draw(3, 0);
    }

    // NVENC maps the target next; leaving it bound as a render target
    // (or the desktop surface bound as an input while capture copies into it)
    // is a hazard the runtime would have to resolve behind our back.
    ID3D11ShaderResourceView* no_srvs[2] = {};
    ctx->PSSetShaderResources(0, 2, no_srvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

} // namespace zcr
