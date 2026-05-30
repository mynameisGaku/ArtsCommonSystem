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

    static constexpr u32 kMaxVerts = 64;
    static constexpr u32 kMaxTris  = 128;

    // ----- 形状指定 (すべて owner 相対) -----
    // 矩形 (海/プール)。上辺を top_segments 分割して滑らかに波打たせる。
    void SetRect(FVec2 center, FVec2 half_size, u32 top_segments = 32) noexcept;
    // 旧 API 互換 (= SetRect)。
    void SetArea(FVec2 center, FVec2 half_size) noexcept { SetRect(center, half_size); }
    // 楕円 (水溜まり / 池)。
    void SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments = 28) noexcept;
    // 川: polyline (path の count 点) を太さ width の帯に膨らませる。
    void SetRiver(const FVec2* path, u32 count, f32 width) noexcept;
    // 任意の単純多角形。centroid から扇状に塗るため凸〜緩い凹向け
    // (強い凹形状は複数の水域に分けて配置するのが安全)。
    void SetPolygon(const FVec2* pts, u32 count) noexcept;

    // スプライン (Catmull-Rom、制御点を通る滑らかな曲線)。少ない制御点で曲がりくねった
    // 川や、滑らかな輪郭の湖を作れる。samples_per_seg で曲線の細かさを指定。
    void SetSplineRiver(const FVec2* control, u32 count, f32 width, u32 samples_per_seg = 8) noexcept;
    void SetSplineRegion(const FVec2* control, u32 count, u32 samples_per_seg = 8) noexcept;  // 閉ループ

    void SetColor(FVec3 rgb) noexcept { m_Color = rgb; }
    void SetAlpha(f32 a) noexcept { m_Alpha = a; }
    void SetWaves(f32 amplitude, f32 speed) noexcept { m_Amp = amplitude; m_Speed = speed; }

    // インタラクション: world_x 付近の水面に強さ strength の波紋を立てる。
    void Disturb(f32 world_x, f32 strength) noexcept;

    // 入水判定ヘルパ。
    bool ContainsPoint(FVec2 world) const noexcept;   // 点が水域内か (point-in-polygon)
    bool ContainsX(f32 world_x) const noexcept;        // bbox の x 範囲内か
    f32  SurfaceY() const noexcept;                    // 水域 bbox 上端の world y

    void OnUpdate(f32 dt) noexcept override;
    void OnDraw(RenderContext& rc) noexcept override;

private:
    void PushVert(FVec2 v) noexcept;
    void PushTri(u32 a, u32 b, u32 c) noexcept;
    void Finish() noexcept;                            // weight / bbox を再計算
    f32  WaveAt(f32 world_x) const noexcept;           // ambient 波 + 波紋

    struct Ripple { f32 x = 0, amp = 0, amp0 = 0, time = 0; bool active = false; };
    static constexpr u32 kMaxRipples = 16;
    Ripple m_Ripples[kMaxRipples];

    FVec2 m_Vert[kMaxVerts];        // owner 相対の頂点
    f32   m_Weight[kMaxVerts];      // 水面らしさ [0,1] (bbox 上端ほど 1)
    u32   m_VCount = 0;
    u16   m_Tri[kMaxTris * 3];      // m_Vert への index (3 個 1 組)
    u32   m_TCount = 0;
    f32   m_MinX = 0, m_MaxX = 0, m_MinY = 0, m_MaxY = 0;

    FVec3 m_Color{0.12f, 0.35f, 0.6f};
    f32   m_Alpha = 0.72f;
    f32   m_Amp   = 0.12f;
    f32   m_Speed = 1.6f;
    f32   m_Time  = 0.0f;
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
