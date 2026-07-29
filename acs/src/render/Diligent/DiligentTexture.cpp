// SPDX-License-Identifier: Apache-2.0
// FDiligentTexture 実装
#include "render/Diligent/DiligentTexture.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

namespace acs {

/** per-slice RTV とテクスチャ本体を解放する (default view は ITexture 所有のため触らない)。 */
FDiligentTexture::~FDiligentTexture() noexcept {
    Reset();
}

void FDiligentTexture::Reset() noexcept
{
    // m_Srv/m_Rtv/m_Dsv は ITexture が所有するビューなので個別 Release は不要。
    // per_slice_rtv で CreateView した別個ビューだけ明示 Release。
    for (usize i = 0, n = m_SliceRtvs.Size(); i < n; ++i) {
        if (m_SliceRtvs[i]) { m_SliceRtvs[i]->Release(); m_SliceRtvs[i] = nullptr; }
    }
    if (m_Texture) { m_Texture->Release(); m_Texture = nullptr; }
    m_SliceRtvs.ReleaseStorage();
    m_Device = nullptr;
    m_Srv = nullptr;
    m_Rtv = nullptr;
    m_Dsv = nullptr;
    m_Uav = nullptr;
    m_IsUav = false;
    m_Depth = 1;
    m_Width = 0;
    m_Height = 0;
    m_Mips = 1;
    m_ArraySize = 1;
    m_Format = EFormat::R8G8B8A8_UNorm;
    m_IsRt = false;
    m_IsDepth = false;
    m_DepthSrv = false;
    m_IsCubemap = false;
}

/** per_slice_rtv で生成した slice/mip 単位の RTV を返す (範囲外なら nullptr)。 */
Diligent::ITextureView* FDiligentTexture::RtvSlice(u32 slice, u32 mip) const noexcept {
    if (m_ArraySize == 0 || m_Mips == 0) return nullptr;
    if (slice >= m_ArraySize || mip >= m_Mips) return nullptr;
    const usize idx = static_cast<usize>(slice) * m_Mips + mip;
    if (idx >= m_SliceRtvs.Size()) return nullptr;
    return m_SliceRtvs[idx];
}

/** 記述に従ってテクスチャ・default view・任意の per-slice RTV を生成する。 */
TResult<void> FDiligentTexture::Init(FDiligentDevice& device, const FTextureDesc& desc) noexcept {
    Reset();

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 130, "FDiligentTexture: device not initialized");
    if (desc.width == 0 || desc.height == 0) {
        return ACS_ERR(Render, 132, "FDiligentTexture: width and height must be non-zero");
    }
    if ((desc.initial_data == nullptr) != (desc.initial_data_size == 0) ||
        (desc.is_depth_target && desc.is_render_target) || (desc.shader_visible_depth && !desc.is_depth_target) ||
        !IsFormatUsageLegal(desc.format, desc.is_depth_target) ||
        (desc.per_slice_rtv && !desc.is_render_target) ||
        (desc.depth > 1 && (desc.array_size > 1 || desc.is_cubemap || desc.per_slice_rtv))) {
        return ACS_ERR(Render, 133, "FDiligentTexture: invalid descriptor combination");
    }
    if (desc.sample_count > 1) {
        // Diligent backend の MSAA resolve 経路は未実装なので、黙って 1 に落とさない。
        return ACS_ERR(Render, 134, "FDiligentTexture: multisampling is not supported yet");
    }
    if (desc.is_cubemap && desc.array_size != 0 && desc.array_size != 6) {
        return ACS_ERR(Render, 135, "FDiligentTexture: is_cubemap=true requires array_size=6 (or 0/default)");
    }

    m_Device  = &device;
    m_Width   = desc.width;
    m_Height  = desc.height;
    m_Mips    = desc.mip_levels > 0 ? desc.mip_levels : 1;
    m_ArraySize = desc.array_size > 0 ? desc.array_size : 1;
    m_Format  = desc.format;
    m_IsRt    = desc.is_render_target;
    m_IsDepth = desc.is_depth_target;
    m_DepthSrv = desc.shader_visible_depth;
    m_IsCubemap = desc.is_cubemap;
    m_IsUav   = desc.is_uav;
    m_Depth   = desc.depth > 0 ? desc.depth : 1;
    if (m_IsCubemap) m_ArraySize = 6;

    Diligent::TextureDesc td;
    td.Name      = "ACS_Texture";
    if (m_Depth > 1)
        td.Type = Diligent::RESOURCE_DIM_TEX_3D;        // volumetric clouds の shape/detail noise 等
    else if (m_IsCubemap)
        td.Type = Diligent::RESOURCE_DIM_TEX_CUBE;
    else if (m_ArraySize > 1)
        td.Type = Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
    else
        td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width     = m_Width;
    td.Height    = m_Height;
    if (m_Depth > 1) td.Depth = m_Depth;                // 3D の奥行き (Diligent では ArraySize と union)
    else td.ArraySize = m_ArraySize;   // cubemap も含めて Diligent はここで指定
    td.Format    = diligent_detail::ToDiligent(m_Format);
    td.MipLevels = m_Mips;
    td.SampleCount = 1;
    td.Usage     = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    if (m_IsRt) {
        td.BindFlags = static_cast<Diligent::BIND_FLAGS>(td.BindFlags | Diligent::BIND_RENDER_TARGET);
    }
    if (m_IsUav) {   // compute から RWTexture として書ける (Phase 0)
        td.BindFlags = static_cast<Diligent::BIND_FLAGS>(td.BindFlags | Diligent::BIND_UNORDERED_ACCESS);
    }
    if (m_IsDepth) {
        td.BindFlags = static_cast<Diligent::BIND_FLAGS>(
            (m_DepthSrv ? Diligent::BIND_SHADER_RESOURCE : Diligent::BIND_NONE) |
            Diligent::BIND_DEPTH_STENCIL);
        // 深度フォーマットが SRV 兼用の場合、TYPELESS を使ったほうがいいケースもあるが
        // Diligent は内部で自動処理してくれる
    }

    Diligent::TextureSubResData sub_data;
    Diligent::TextureData       initial;
    Diligent::TextureData*      p_init = nullptr;
    if (desc.initial_data && desc.initial_data_size > 0
        && (m_ArraySize > 1 || m_Mips > 1) && !m_IsDepth && !m_IsRt) {
        // cubemap / array / mip > 1 への initial_data は現状未サポート
        // (GPU 上で焼く設計に倒している)。silent drop を防ぐため明示的に警告する。
        ACS_LOG_WARN("FDiligentTexture: initial_data ignored for array_size=%u mips=%u",
                     m_ArraySize, m_Mips);
    }
    if (desc.initial_data && desc.initial_data_size > 0 && !m_IsDepth && !m_IsRt
        && m_ArraySize == 1 && m_Mips == 1) {
        sub_data.pData       = desc.initial_data;
        sub_data.Stride      = static_cast<Diligent::Uint64>(m_Width) * 4;  // RGBA8 想定（32bpp）
        // フォーマットによって stride を補正
        if (m_Format == EFormat::R32G32B32A32_Float)      sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 16;
        else if (m_Format == EFormat::R16G16B16A16_Float) sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 8;
        else if (m_Format == EFormat::R32G32B32_Float)    sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 12;
        else if (m_Format == EFormat::R32G32_Float)       sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 8;
        else if (m_Format == EFormat::R11G11B10_Float)    sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 4;
        else if (m_Format == EFormat::R16G16_Float)       sub_data.Stride = static_cast<Diligent::Uint64>(m_Width) * 4;
        initial.pSubResources   = &sub_data;
        initial.NumSubresources = 1;
        p_init = &initial;
    }

    dev->CreateTexture(td, p_init, &m_Texture);
    if (!m_Texture) {
        Reset();
        return ACS_ERR(Render, 131, "CreateTexture failed");
    }

    // デフォルトビューを取り出す（ITexture が所有しているので参照だけ保持）
    if (td.BindFlags & Diligent::BIND_SHADER_RESOURCE) {
        m_Srv = m_Texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    }
    if (td.BindFlags & Diligent::BIND_RENDER_TARGET) {
        m_Rtv = m_Texture->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    }
    if (td.BindFlags & Diligent::BIND_DEPTH_STENCIL) {
        m_Dsv = m_Texture->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    }
    if (td.BindFlags & Diligent::BIND_UNORDERED_ACCESS) {
        m_Uav = m_Texture->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS);
    }

    // per-slice RTV を要求された場合は array_size × mip_levels 個生成する。
    // cubemap face / 2D array slice / mip 単位の描画パスで使う (IBL prefilter 等)。
    if (m_IsRt && desc.per_slice_rtv) {
        const usize total = static_cast<usize>(m_ArraySize) * m_Mips;
        m_SliceRtvs.Resize(total);
        for (u32 s = 0; s < m_ArraySize; ++s) {
            for (u32 m = 0; m < m_Mips; ++m) {
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
                m_Texture->CreateView(vd, &view);
                if (!view) {
                    Reset();
                    return ACS_ERR(Render, 136, "FDiligentTexture: CreateView failed");
                }
                m_SliceRtvs[static_cast<usize>(s) * m_Mips + m] = view;
            }
        }
    }

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
