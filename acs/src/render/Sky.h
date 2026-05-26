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
    void SetSunColor(FVec3 c)       noexcept { _sun_color = c; }
    void SetSunRadius(f32 angular) noexcept { _sun_radius  = angular; }   // 視線角の cos 値からの差（0.001 = 鋭い、0.05 = 大きい）
    void SetSunGlow(f32 angular)   noexcept { _sun_glow    = angular; }   // 太陽の周りのハロー
    void SetZenithColor(FVec3 c)    noexcept { _zenith  = c; }   // 天頂の色
    void SetHorizonColor(FVec3 c)   noexcept { _horizon = c; }   // 地平線の色
    void SetGroundColor(FVec3 c)    noexcept { _ground  = c; }   // 地面方向の色

    // プリセット
    void PresetDay()    noexcept;    // 青空 + 白い太陽
    void PresetSunset() noexcept;    // 茜色 + 暖色太陽
    void PresetNight()  noexcept;    // 紺青 + 弱い月光

    // 現在の太陽パラメータ取得（FStandardShader / IBL と整合させたいときに）
    FVec3 SunDirection() const noexcept { return _sun_dir; }
    FVec3 SunColor()     const noexcept { return _sun_color; }
    f32  SunRadius()    const noexcept { return _sun_radius; }
    f32  SunGlow()      const noexcept { return _sun_glow; }
    FVec3 ZenithColor()  const noexcept { return _zenith; }
    FVec3 HorizonColor() const noexcept { return _horizon; }
    FVec3 GroundColor()  const noexcept { return _ground; }

    // 描画（先頭で呼ぶ。深度バッファは「背景に塗る」想定で書込み無し・テスト無し）
    void Render(IRhiCommandList& cl, const FCamera& camera) noexcept;

private:
    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _cb;

    FVec3 _sun_dir    = FVec3{0.5f, 0.8f, 0.3f};
    FVec3 _sun_color  = FVec3{1.0f, 0.95f, 0.85f};
    f32  _sun_radius = 0.0006f;
    f32  _sun_glow   = 0.04f;
    FVec3 _zenith     = FVec3{0.18f, 0.40f, 0.78f};
    FVec3 _horizon    = FVec3{0.70f, 0.80f, 0.95f};
    FVec3 _ground     = FVec3{0.20f, 0.18f, 0.16f};
};

} // namespace acs
