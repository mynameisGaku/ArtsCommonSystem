// 2D テクスチャ抽象（GPU 上の画像。シェーダから読める SRV を持つ）
//
// 使い方:
//   TextureDesc d{};
//   d.width = 256; d.height = 256;
//   d.format = Format::R8G8B8A8_UNorm;
//   d.initial_data = pixels;          // RGBA 8bit、上から下、左から右の順
//   d.initial_data_size = 256*256*4;
//   auto tex = CreateRhiTexture(device, d).Value();
//
// 描画時:
//   cmd.SetTexture(0, *tex);          // パイプラインで texture_slots>=1 が必要
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;

struct TextureDesc {
    u32         width            = 0;
    u32         height           = 0;
    Format      format           = Format::R8G8B8A8_UNorm;
    u32         mip_levels       = 1;             // 1 = ベースのみ
    bool        is_render_target = false;          // RT として使うなら true（v1 では未使用）
    bool        is_depth_target  = false;          // 深度バッファとして使うなら true
    bool        shader_visible_depth = false;     // is_depth_target=true で SRV も作る（シャドウマップ用）
    const void* initial_data     = nullptr;        // RGBA 等、tightly-packed
    usize       initial_data_size = 0;
};

class IRhiTexture {
public:
    virtual ~IRhiTexture() noexcept = default;

    virtual u32    Width()      const noexcept = 0;
    virtual u32    Height()     const noexcept = 0;
    virtual Format PixelFormat() const noexcept = 0;
};

Result<UniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                     const TextureDesc& desc) noexcept;

} // namespace acs
