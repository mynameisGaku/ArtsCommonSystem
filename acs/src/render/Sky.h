// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ（グラデーション + 太陽）
//
// 用途: 3D シーンの背景に「空」を描く。シーン描画より先に呼ぶ。
//       テクスチャ（キューブマップ）不要、ピクセルシェーダで天頂・地平線・
//       地面の色を補間して描画する。
//
// 使い方:
//   FSky sky;
//   sky.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
//   sky.PresetDay();
//
//   // 描画フレーム中、シーンの最初に
//   sky.Render(*cl, camera);
//   // ... FStandardShader でメッシュを描く ...
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"

namespace acs {

class FCamera;

class FSky {
public:
    FSky() noexcept = default;
    ~FSky() noexcept = default;

    FSky(const FSky&)            = delete;
    FSky& operator=(const FSky&) = delete;

    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;
    void Shutdown() noexcept;

    // パラメータ設定（カメラと同じ右手 / 左手系の前提なし、シェーダで normalize する）
    void SetSunDirection(FVec3 dir) noexcept;       // 原点から太陽へ向く方向
    void SetSunColor(FVec3 c)       noexcept { m_SunColor = c; }
    void SetSunRadius(f32 angular) noexcept { m_SunRadius  = angular; }   // 視線角の cos 値からの差（0.001 = 鋭い、0.05 = 大きい）
    void SetSunGlow(f32 angular)   noexcept { m_SunGlow    = angular; }   // 太陽の周りのハロー
    void SetZenithColor(FVec3 c)    noexcept { m_Zenith  = c; }   // 天頂の色
    void SetHorizonColor(FVec3 c)   noexcept { m_Horizon = c; }   // 地平線の色
    void SetGroundColor(FVec3 c)    noexcept { m_Ground  = c; }   // 地面方向の色

    // プリセット
    void PresetDay()    noexcept;    // 青空 + 白い太陽
    void PresetSunset() noexcept;    // 茜色 + 暖色太陽
    void PresetNight()  noexcept;    // 紺青 + 弱い月光

    // 現在の太陽パラメータ取得（FStandardShader / IBL と整合させたいときに）
    FVec3 SunDirection() const noexcept { return m_SunDir; }
    FVec3 SunColor()     const noexcept { return m_SunColor; }
    f32  SunRadius()    const noexcept { return m_SunRadius; }
    f32  SunGlow()      const noexcept { return m_SunGlow; }
    FVec3 ZenithColor()  const noexcept { return m_Zenith; }
    FVec3 HorizonColor() const noexcept { return m_Horizon; }
    FVec3 GroundColor()  const noexcept { return m_Ground; }

    // 描画（先頭で呼ぶ。深度バッファは「背景に塗る」想定で書込み無し・テスト無し）
    void Render(IRhiCommandList& cl, const FCamera& camera) noexcept;

private:
    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiBuffer>   m_Cb;

    FVec3 m_SunDir    = FVec3{0.5f, 0.8f, 0.3f};
    FVec3 m_SunColor  = FVec3{1.0f, 0.95f, 0.85f};
    f32  m_SunRadius = 0.0006f;
    f32  m_SunGlow   = 0.04f;
    FVec3 m_Zenith     = FVec3{0.18f, 0.40f, 0.78f};
    FVec3 m_Horizon    = FVec3{0.70f, 0.80f, 0.95f};
    FVec3 m_Ground     = FVec3{0.20f, 0.18f, 0.16f};
};

} // namespace acs
