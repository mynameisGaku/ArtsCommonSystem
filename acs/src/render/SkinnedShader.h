// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ用ライティングシェーダ（GPU スキニング）
//
// StandardShader の上位互換: PerFrame (b0) / PerObject (b1) は同じレイアウト。
// 加えて Bones (b2) を持ち、最大 64 ボーンのパレット行列をシェーダに送る。
//
// 使い方:
//   SkinnedShader shd;
//   shd.Init(*dev, color_fmt, depth_fmt);
//
//   // フレーム共通（StandardShader と同じ呼び方）
//   shd.SetLights(camera.ViewProjection(), camera.Eye(),
//                 lights, count, ambient);
//
//   // オブジェクト
//   shd.SetObject(model_mat, base_color, specular, shininess);
//
//   // ボーンパレット（毎フレーム）
//   FMat4 palette[64];
//   u32 nb = anim_player.WritePalette(palette, 64);
//   shd.SetBonePalette(palette, nb);
//
//   cl->SetPipeline(*shd.Pipeline());
//   cl->SetConstantBuffer(0, *shd.PerFrameCB());
//   cl->SetConstantBuffer(1, *shd.PerObjectCB());
//   cl->SetConstantBuffer(2, *shd.BonesCB());
//   cl->SetTexture(0, *shd.DefaultWhiteTexture());
//   cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
//   cl->SetIndexBuffer (*gm.index_buffer);
//   cl->DrawIndexed(gm.index_count);
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"
#include "render/StandardShader.h"   // DirLight 共有

namespace acs {

class SkinnedShader {
public:
    static constexpr u32 kMaxBones = 64;

    SkinnedShader() noexcept = default;
    ~SkinnedShader() noexcept = default;

    SkinnedShader(const SkinnedShader&)            = delete;
    SkinnedShader& operator=(const SkinnedShader&) = delete;

    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;
    void Shutdown() noexcept;

    // StandardShader と同形式の API（互換）
    void SetFrame(const FMat4& view_projection,
                  FVec3 camera_pos,
                  FVec3 light_dir, FVec3 light_color,
                  FVec3 ambient_color) noexcept;
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const DirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;
    void SetPointLights(const PointLight* lights, u32 count) noexcept;
    void SetObject(const FMat4& model,
                   FVec3 base_color = FVec3{1, 1, 1},
                   f32  specular_strength = 0.0f,
                   f32  shininess = 32.0f) noexcept;

    // ボーンパレット（最大 kMaxBones 個）。残りは内部で単位行列で埋める。
    void SetBonePalette(const FMat4* palette, u32 count) noexcept;

    IRhiPipeline*  Pipeline()    const noexcept { return _pipeline.Get(); }
    IRhiBuffer*    PerFrameCB()  const noexcept { return _frame_cb.Get(); }
    IRhiBuffer*    PerObjectCB() const noexcept { return _object_cb.Get(); }
    IRhiBuffer*    BonesCB()     const noexcept { return _bones_cb.Get(); }
    IRhiTexture*   DefaultWhiteTexture() const noexcept { return _white.Get(); }

private:
    void FlushFrameCB() noexcept;

    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _frame_cb;
    TUniquePtr<IRhiBuffer>   _object_cb;
    TUniquePtr<IRhiBuffer>   _bones_cb;
    TUniquePtr<IRhiTexture>  _white;

    // Frame の状態キャッシュ（StandardShader と同パターン）
    FMat4       _vp;
    FVec3       _eye = FVec3{0, 0, 0};
    FVec3       _ambient = FVec3{0, 0, 0};
    DirLight   _dir_lights[4];
    u32        _dir_count = 0;
    PointLight _point_lights[4];
    u32        _point_count = 0;
};

} // namespace acs
