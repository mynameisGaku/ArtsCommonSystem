// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ用ライティングシェーダ（GPU スキニング）
//
// FStandardShader の上位互換: PerFrame (b0) / PerObject (b1) は同じレイアウト。
// 加えて Bones (b2) を持ち、最大 64 ボーンのパレット行列をシェーダに送る。
//
// 使い方:
//   FSkinnedShader shd;
//   shd.Init(*dev, color_fmt, depth_fmt);
//
//   // フレーム共通（FStandardShader と同じ呼び方）
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

class FSkinnedShader {
public:
    static constexpr u32 kMaxBones = 64;

    FSkinnedShader() noexcept = default;
    ~FSkinnedShader() noexcept = default;

    FSkinnedShader(const FSkinnedShader&)            = delete;
    FSkinnedShader& operator=(const FSkinnedShader&) = delete;

    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;
    void Shutdown() noexcept;

    // FStandardShader と同形式の API（互換）
    void SetFrame(const FMat4& view_projection,
                  FVec3 camera_pos,
                  FVec3 light_dir, FVec3 light_color,
                  FVec3 ambient_color) noexcept;
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const FDirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;
    void SetPointLights(const PointLight* lights, u32 count) noexcept;
    void SetObject(const FMat4& model,
                   FVec3 base_color = FVec3{1, 1, 1},
                   f32  specular_strength = 0.0f,
                   f32  shininess = 32.0f) noexcept;

    // ボーンパレット（最大 kMaxBones 個）。残りは内部で単位行列で埋める。
    void SetBonePalette(const FMat4* palette, u32 count) noexcept;

    IRhiPipeline*  Pipeline()    const noexcept { return m_Pipeline.Get(); }
    IRhiBuffer*    PerFrameCB()  const noexcept { return m_FrameCb.Get(); }
    IRhiBuffer*    PerObjectCB() const noexcept { return m_ObjectCb.Get(); }
    IRhiBuffer*    BonesCB()     const noexcept { return m_BonesCb.Get(); }
    IRhiTexture*   DefaultWhiteTexture() const noexcept { return m_White.Get(); }

private:
    void FlushFrameCB() noexcept;

    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiBuffer>   m_FrameCb;
    TUniquePtr<IRhiBuffer>   m_ObjectCb;
    TUniquePtr<IRhiBuffer>   m_BonesCb;
    TUniquePtr<IRhiTexture>  m_White;

    // Frame の状態キャッシュ（FStandardShader と同パターン）
    FMat4       m_Vp;
    FVec3       m_Eye = FVec3{0, 0, 0};
    FVec3       m_Ambient = FVec3{0, 0, 0};
    FDirLight   m_DirLights[4];
    u32        m_DirCount = 0;
    PointLight m_PointLights[4];
    u32        m_PointCount = 0;
};

} // namespace acs
