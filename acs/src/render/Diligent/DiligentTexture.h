// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のテクスチャ
#pragma once

#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace Diligent {
    struct ITexture;
    struct ITextureView;
}

namespace acs {

class DiligentDevice;

class DiligentTexture final : public IRhiTexture {
public:
    DiligentTexture() noexcept = default;
    ~DiligentTexture() noexcept override;

    DiligentTexture(const DiligentTexture&) = delete;
    DiligentTexture& operator=(const DiligentTexture&) = delete;

    TResult<void> Init(DiligentDevice& device, const FTextureDesc& desc) noexcept;

    // ---- IRhiTexture ----
    u32    Width()       const noexcept override { return m_Width; }
    u32    Height()      const noexcept override { return m_Height; }
    EFormat EPixelFormat() const noexcept override { return m_Format; }
    u32    MipLevels()   const noexcept override { return m_Mips; }
    u32    ArraySize()   const noexcept override { return m_ArraySize; }
    bool   IsCubemap()   const noexcept override { return m_IsCubemap; }

    // 内部公開
    Diligent::ITexture*     Native()    const noexcept { return m_Texture; }
    Diligent::ITextureView* SrvView()   const noexcept { return m_Srv; }
    Diligent::ITextureView* RtvView()   const noexcept { return m_Rtv; }
    Diligent::ITextureView* DsvView()   const noexcept { return m_Dsv; }

    // per_slice_rtv で生成した slice/mip ごとの RTV を取得 (見つからなければ nullptr)
    Diligent::ITextureView* RtvSlice(u32 slice, u32 mip) const noexcept;

    bool IsRenderTarget()    const noexcept { return m_IsRt; }
    bool IsDepthTarget()     const noexcept { return m_IsDepth; }
    // 深度バッファが stencil 成分を持つか (D24_UNorm_S8_UInt)。clear で stencil
    // フラグを足すかの判定に使う。
    bool HasStencil()        const noexcept {
        return m_IsDepth && m_Format == EFormat::D24_UNorm_S8_UInt;
    }
    bool ShaderVisibleDepth() const noexcept { return m_DepthSrv; }

private:
    DiligentDevice*         m_Device  = nullptr;
    Diligent::ITexture*     m_Texture = nullptr;
    Diligent::ITextureView* m_Srv     = nullptr;  // not addref'd; owned by m_Texture
    Diligent::ITextureView* m_Rtv     = nullptr;
    Diligent::ITextureView* m_Dsv     = nullptr;
    // per_slice_rtv=true のとき、array_size*mip_levels 個の RTV を Init で生成して
    // ここに保持する。これらは ITexture::CreateView で別途生成されるので
    // 明示 Release が必要 (m_Texture 所有ではない default view と異なる)。
    TArray<Diligent::ITextureView*> m_SliceRtvs;
    u32                     m_Width   = 0;
    u32                     m_Height  = 0;
    u32                     m_Mips    = 1;
    u32                     m_ArraySize = 1;
    EFormat                  m_Format  = EFormat::R8G8B8A8_UNorm;
    bool                    m_IsRt    = false;
    bool                    m_IsDepth = false;
    bool                    m_DepthSrv = false;
    bool                    m_IsCubemap = false;
};

} // namespace acs
