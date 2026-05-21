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

    Result<void> Init(DiligentDevice& device, const TextureDesc& desc) noexcept;

    // ---- IRhiTexture ----
    u32    Width()       const noexcept override { return _width; }
    u32    Height()      const noexcept override { return _height; }
    Format PixelFormat() const noexcept override { return _format; }
    u32    MipLevels()   const noexcept override { return _mips; }
    u32    ArraySize()   const noexcept override { return _array_size; }
    bool   IsCubemap()   const noexcept override { return _is_cubemap; }

    // 内部公開
    Diligent::ITexture*     Native()    const noexcept { return _texture; }
    Diligent::ITextureView* SrvView()   const noexcept { return _srv; }
    Diligent::ITextureView* RtvView()   const noexcept { return _rtv; }
    Diligent::ITextureView* DsvView()   const noexcept { return _dsv; }

    // per_slice_rtv で生成した slice/mip ごとの RTV を取得 (見つからなければ nullptr)
    Diligent::ITextureView* RtvSlice(u32 slice, u32 mip) const noexcept;

    bool IsRenderTarget()    const noexcept { return _is_rt; }
    bool IsDepthTarget()     const noexcept { return _is_depth; }
    bool ShaderVisibleDepth() const noexcept { return _depth_srv; }

private:
    DiligentDevice*         _device  = nullptr;
    Diligent::ITexture*     _texture = nullptr;
    Diligent::ITextureView* _srv     = nullptr;  // not addref'd; owned by _texture
    Diligent::ITextureView* _rtv     = nullptr;
    Diligent::ITextureView* _dsv     = nullptr;
    // per_slice_rtv=true のとき、array_size*mip_levels 個の RTV を Init で生成して
    // ここに保持する。これらは ITexture::CreateView で別途生成されるので
    // 明示 Release が必要 (_texture 所有ではない default view と異なる)。
    Array<Diligent::ITextureView*> _slice_rtvs;
    u32                     _width   = 0;
    u32                     _height  = 0;
    u32                     _mips    = 1;
    u32                     _array_size = 1;
    Format                  _format  = Format::R8G8B8A8_UNorm;
    bool                    _is_rt    = false;
    bool                    _is_depth = false;
    bool                    _depth_srv = false;
    bool                    _is_cubemap = false;
};

} // namespace acs
