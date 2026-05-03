// DiligentTexture 実装
#include "render/Diligent/DiligentTexture.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

namespace acs {

DiligentTexture::~DiligentTexture() noexcept {
    // _srv/_rtv/_dsv は ITexture が所有するビューなので個別 Release は不要
    if (_texture) { _texture->Release(); _texture = nullptr; }
}

Result<void> DiligentTexture::Init(DiligentDevice& device, const TextureDesc& desc) noexcept {
    _device  = &device;
    _width   = desc.width;
    _height  = desc.height;
    _mips    = desc.mip_levels > 0 ? desc.mip_levels : 1;
    _format  = desc.format;
    _is_rt    = desc.is_render_target;
    _is_depth = desc.is_depth_target;
    _depth_srv = desc.shader_visible_depth;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 130, "DiligentTexture: device not initialized");

    Diligent::TextureDesc td;
    td.Name      = "ACS_Texture";
    td.Type      = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width     = _width;
    td.Height    = _height;
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
    if (desc.initial_data && desc.initial_data_size > 0 && !_is_depth && !_is_rt) {
        sub_data.pData       = desc.initial_data;
        sub_data.Stride      = static_cast<Diligent::Uint64>(_width) * 4;  // RGBA8 想定（32bpp）
        // フォーマットによって stride を補正
        if (_format == Format::R32G32B32A32_Float)      sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 16;
        else if (_format == Format::R16G16B16A16_Float) sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 8;
        else if (_format == Format::R32G32B32_Float)    sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 12;
        else if (_format == Format::R32G32_Float)       sub_data.Stride = static_cast<Diligent::Uint64>(_width) * 8;
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

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
