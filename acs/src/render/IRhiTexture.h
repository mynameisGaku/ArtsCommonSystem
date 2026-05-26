// SPDX-License-Identifier: Apache-2.0
// 2D テクスチャ抽象（GPU 上の画像。シェーダから読める SRV を持つ）
//
// 使い方:
//   TextureDesc d{};
//   d.width = 256; d.height = 256;
//   d.format = EFormat::R8G8B8A8_UNorm;
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
    EFormat      format           = EFormat::R8G8B8A8_UNorm;
    u32         mip_levels       = 1;             // 1 = ベースのみ。>1 で生成する場合は per_slice_rtv を併用してミップ毎に描画する
    u32         array_size       = 1;             // 1 = 単一、6 = cubemap、>1 = テクスチャ配列
    bool        is_cubemap       = false;         // true なら array_size==6 を要求（Diligent: RESOURCE_DIM_TEX_CUBE）
    bool        is_render_target = false;          // RT として使うなら true
    bool        is_depth_target  = false;          // 深度バッファとして使うなら true
    bool        shader_visible_depth = false;     // is_depth_target=true で SRV も作る（シャドウマップ用）
    bool        per_slice_rtv    = false;         // is_render_target=true 時、array_size*mip_levels 個の RTV を per-slice 作成（cubemap 面/ミップ別パスを書くのに必要）
    const void* initial_data     = nullptr;        // RGBA 等、tightly-packed（cubemap/array 初期化は未サポート）
    usize       initial_data_size = 0;
};

class IRhiTexture {
public:
    virtual ~IRhiTexture() noexcept = default;

    virtual u32    Width()      const noexcept = 0;
    virtual u32    Height()     const noexcept = 0;
    virtual EFormat EPixelFormat() const noexcept = 0;

    // Phase 31 で追加。既存バックエンドは安全な既定値を返してよい。
    virtual u32    MipLevels()  const noexcept { return 1; }
    virtual u32    ArraySize()  const noexcept { return 1; }
    virtual bool   IsCubemap()  const noexcept { return false; }
};

TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                     const TextureDesc& desc) noexcept;

} // namespace acs
