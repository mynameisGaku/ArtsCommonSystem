// SPDX-License-Identifier: Apache-2.0
// Effects2D — シェーダー無し・アセット無しの drop-in エフェクトコンポーネント集。
//
// すべて FSpriteBatch のプリミティブ (三角形/矩形) を時間で手続き生成して描く。
// HLSL を書けなくても AddComponent + パラメータ設定だけで動く。インタラクティブな
// API (Disturb / SetIntensity / owner 追従) を持ち、入力や物理と連動できる。
//
//   auto& w = node->AddComponent<FWater2DComponent>();
//   w.SetArea({0,-3}, {16,2}); w.SetColor({0.1f,0.35f,0.6f}); w.SetWaves(0.15f,1.5f);
//   w.Disturb(mouse_world_x, 0.4f);     // ← 触れると波紋
//
//   auto& f = node->AddComponent<FFire2DComponent>();
//   f.SetSize(0.8f,1.8f); f.SetIntensity(1.0f);   // ← 近づくと勢い UP 等
//
//   auto& t = node->AddComponent<FTrail2DComponent>();  // owner を追従して残像
//   t.SetColor({0.3f,0.8f,1.0f}); t.SetWidth(0.3f);
#pragma once

#include "gameframework/Component2D.h"
#include "math/Vec.h"

namespace acs::game {

// ===========================================================================
// FWater2DComponent — 波打つ水面 + 干渉 (ripple / splash)
// ===========================================================================
class FWater2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FWater2DComponent)

    // owner 相対の矩形領域 (center = 中心、half_size = 半径)。上辺が水面。
    void SetArea(FVec2 center, FVec2 half_size) noexcept { m_Center = center; m_Half = half_size; }
    void SetColor(FVec3 rgb) noexcept { m_Color = rgb; }
    void SetAlpha(f32 a) noexcept { m_Alpha = a; }
    void SetWaves(f32 amplitude, f32 speed) noexcept { m_Amp = amplitude; m_Speed = speed; }
    void SetSegments(u32 n) noexcept { m_Segs = n < 4u ? 4u : (n > 128u ? 128u : n); }

    // インタラクション: 水面上の world_x に強さ strength の波紋を立てる。
    // (例: マウスが水面をなぞる / 物体が入水した瞬間に呼ぶ)
    void Disturb(f32 world_x, f32 strength) noexcept;

    // 「入水」判定ヘルパ: 水面の world y、x が領域内か、点が水域内か。
    f32  SurfaceY() const noexcept;
    bool ContainsX(f32 world_x) const noexcept;
    bool ContainsPoint(FVec2 world) const noexcept;   // 水域矩形内か (上下方向非依存)

    void OnUpdate(f32 dt) noexcept override;
    void OnDraw(RenderContext& rc) noexcept override;

private:
    f32 SurfaceOffsetAt(f32 world_x) const noexcept;   // ambient 波 + 波紋の合成変位

    struct Ripple { f32 x = 0, amp = 0, amp0 = 0, time = 0; bool active = false; };
    static constexpr u32 kMaxRipples = 16;
    Ripple m_Ripples[kMaxRipples];

    FVec2 m_Center{0.0f, 0.0f};
    FVec2 m_Half{4.0f, 1.0f};
    FVec3 m_Color{0.12f, 0.35f, 0.6f};
    f32   m_Alpha = 0.72f;
    f32   m_Amp   = 0.12f;
    f32   m_Speed = 1.6f;
    f32   m_Time  = 0.0f;
    u32   m_Segs  = 56;
};

// ===========================================================================
// FFire2DComponent — 手続き炎 (ちらつき + 揺らぎ、強さを動的制御可)
// ===========================================================================
class FFire2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FFire2DComponent)

    void SetSize(f32 width, f32 height) noexcept { m_W = width; m_H = height; }
    void SetIntensity(f32 i) noexcept { m_Intensity = i < 0.0f ? 0.0f : i; }  // インタラクション
    f32  Intensity() const noexcept { return m_Intensity; }

    void OnUpdate(f32 dt) noexcept override { m_Time += dt; }
    void OnDraw(RenderContext& rc) noexcept override;

private:
    f32 m_W = 0.8f, m_H = 1.6f, m_Intensity = 1.0f, m_Time = 0.0f;
};

// ===========================================================================
// FTrail2DComponent — owner ノードを追従するモーショントレイル (残像)
// ===========================================================================
class FTrail2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FTrail2DComponent)

    void SetColor(FVec3 rgb) noexcept { m_Color = rgb; }
    void SetWidth(f32 w) noexcept { m_Width = w; }
    void SetPoints(u32 k) noexcept { m_Max = k < 2u ? 2u : (k > kCap ? kCap : k); }

    void OnUpdate(f32 dt) noexcept override;
    void OnDraw(RenderContext& rc) noexcept override;

private:
    static constexpr u32 kCap = 48;
    FVec2 m_Pts[kCap];                 // FIFO: m_Pts[0] = 最新 (head)
    u32   m_Count = 0;
    FVec3 m_Color{0.3f, 0.8f, 1.0f};
    f32   m_Width = 0.25f;
    f32   m_SampleAccum = 0.0f;
    u32   m_Max = 24;
};

} // namespace acs::game
