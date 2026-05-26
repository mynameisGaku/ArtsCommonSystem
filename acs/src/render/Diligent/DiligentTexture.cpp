// SPDX-License-Identifier: Apache-2.0
// DiligentTexture 実装
#include "render/Diligent/DiligentTexture.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

namespace acs {

DiligentTexture::~DiligentTexture() noexcept {
    // _srv/_rtv/_dsv は ITexture が所有するビューなので個別 Release は不要。
    // per_slice_rtv で CreateView した別個ビューだけ明示 Release。
    for (usize i = 0, n = _slice_rtvs.Size(); i < n; ++i) {
        if (_slice_rtvs[i]) { _slice_rtvs[i]->Release(); _slice_rtvs[i] = nullptr; }
    }
    if (_texture) { _texture->Release(); _texture = nullptr; }
}

Diligent::ITextureView* DiligentTexture::RtvSlice(u32 slice, u32 mip) const noexcept {
    if (_array_size == 0 || _mips == 0) return nullptr;
    if (slice >= _array_size || mip >= _mips) return nullptr;
    const usize idx = static_cast<usize>(slice) * _mips + mip;
    if (idx >= _slice_rtvs.Size()) return nullptr;
    return _slice_rtvs[idx];
}

TResult<void> DiligentTexture::Init(DiligentDevice& device, const FTextureDesc& desc) noexcept {
    _device  = &device;
    _width   = desc.width;
    _height  = desc.height;
    _mips    = desc.mip_levels > 0 ? desc.mip_levels : 1;
    _array_size = desc.array_size > 0 ? desc.array_size : 1;
    _format  = desc.format;
    _is_rt    = desc.is_render_target;
    _is_depth = desc.is_depth_target;
    _depth_srv = desc.shader_visible_depth;
    _is_cubemap = desc.is_cubemap;
    if (_is_cubemap) {
        // cubemap は array_size 6 のみ許可。0 (= デフォルト省略) は許容して 6 に正規化。
        if (desc.array_size != 0 && desc.array_size != 6) {
            return ACS_ERR(Render, 132,
                "DiligentTexture: is_cubemap=true requires array_size=6 (or 0/default)");
        }
        _array_size = 6;
    }

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 130, "DiligentTexture: device not initialized");

    Diligent::TextureDesc td;
    td.Name      = "ACS_Texture";
    if (_is_cubemap)
        td.Type = Diligent::RESOURCE_DIM_TEX_CUBE;
    else if (_array_size > 1)
        td.Type = Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
    else
        td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width     = _width;
    td.Height    = _height;
    td.ArraySize = _array_size;        // cubemap も含めて Diligent はここで指定
    td.Format    = diligent_detail::ToDiligent(_format);
    td.MipLevels = _mips;
    td.SampleCount = 1;
    td.Usage     = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    if (_is_rt) {
        td.BindFlags = static_cast<Diligent::BIND_FLAGS>(td.BindFlags | Diligent::BIND_RENDER_TARGET);
    }
    if (_is_depth) {
        td.BindFlags = static_cast<Diligent::BIND_FLAGS>(
            (_depth_srv ? Diligent::BIND_SHADER_RESOURCE : Diligent::BIND_NONE) |
            Diligent::BIND_DEPTH_STENCIL);
        // 深度フォーマットが SRV 兼用の場合、TYPELESS を使ったほうがいいケースもあるが
        // Diligent は内部で自動処理してくれる
    }

    Diligent::TextureSubResData sub_data;
    Diligent::TextureData       initial;
    Diligent::TextureData*      p_init = nullptr;
    if (desc.initial_data && desc.initial_data_size > 0
        && (_array_size > 1 || _mips > 1) && !_is_depth && !_is_rt) {
        // cubemap / array / mip > 1 への initial_data は現状未サポート (Phase 31 では
        // GPU 上で焼く設計に倒している)。silent drop を防ぐため明示的に警告する。
        ACS_LOG_WARN("DiligentTexture: initial_data ignored for array_size=%u mips=%u",
                     _array_size, _mips);
    }
    if (desc.initial_data && desc.initial_data_size > 0 && !_is_depth && !_is_rt
        && _array_size == 1 && _mips == 1) {
        sub_data.pData       = desc.initial_data;
        sub_data.Stride      = static_cast<Diligent::Uint64>(_width) * 4;  // RGBA8 想定（32bpp）
        // フォーマットによって stride を補正
        if (_format == EFormat::R32G32B32A32_Float)      sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 16;
        else if (_format == EFormat::R16G16B16A16_Float) sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 8;
        else if (_format == EFormat::R32G32B32_Float)    sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 12;
        else if (_format == EFormat::R32G32_Float)       sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 8;
        else if (_format == EFormat::R11G11B10_Float)    sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 4;
        else if (_format == EFormat::R16G16_Float)       sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 4;
        initial.pSubResources   = &sub_data;
        initial.NumSubresources = 1;
        p_init = &initial;
    }

    dev->CreateTexture(td, p_init, &_texture);
    if (!_texture) {
        return ACS_ERR(Render, 131, "CreateTexture failed");
    }

    // デフォルトビューを取り出す（ITexture が所有しているので参照だけ保持）
    if (td.BindFlags & Diligent::BIND_SHADER_RESOURCE) {
        _srv = _texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    }
    if (td.BindFlags & Diligent::BIND_RENDER_TARGET) {
        _rtv = _texture->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    }
    if (td.BindFlags & Diligent::BIND_DEPTH_STENCIL) {
        _dsv = _texture->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    }

    // per-slice RTV を要求された場合は array_size × mip_levels 個生成する。
    // cubemap face / 2D array slice / mip 単位の描画パスで使う (IBL prefilter 等)。
    if (_is_rt && desc.per_slice_rtv) {
        const usize total = static_cast<usize>(_array_size) * _mips;
        _slice_rtvs.Resize(total);
        for (u32 s = 0; s < _array_size; ++s) {
            for (u32 m = 0; m < _mips; ++m) {
                Diligent::TextureViewDesc vd;
                vd.ViewType        = Diligent::TEXTURE_VIEW_RENDER_TARGET;
                // cubemap でも array RTV として slice を指す (Diligent 推奨)
                vd.TextureDim      = Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
                vd.Format          = td.Format;
                vd.MostDetailedMip = m;
                vd.NumMipLevels    = 1;
                vd.FirstArraySlice = s;
                vd.NumArraySlices  = 1;
                Diligent::ITextureView* view = nullptr;
                _texture->CreateView(vd, &view);
                _slice_rtvs[static_cast<usize>(s) * _mips + m] = view;
            }
        }
    }

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
