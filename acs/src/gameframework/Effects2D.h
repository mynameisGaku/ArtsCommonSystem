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

    // 後方互換: SetColor は深さ勾配の shallow/deep を自動設定 (deep = rgb*0.45)。
    void SetColor(FVec3 rgb) noexcept {
        m_Color = rgb; m_ShallowColor = rgb;
        m_DeepColor = FVec3{ rgb.x * 0.45f, rgb.y * 0.45f, rgb.z * 0.45f };
    }
    void SetAlpha(f32 a) noexcept { m_Alpha = a; m_ShallowAlpha = a; m_DeepAlpha = a; }
    void SetWaves(f32 amplitude, f32 speed) noexcept { m_Amp = amplitude; m_Speed = speed; m_FlowSpeed = speed; }

    // 流れる方向: 波(と泡)が dir 方向へ流れる。川は流れに沿った dir を指定。
    // dir は内部で正規化。speed は流れの速さ。(既定 = +X 方向)
    void SetFlow(FVec2 dir, f32 speed) noexcept;

    // ----- 深さ / 泡 / 縁 (HLSL 不要、頂点カラー勾配 + 境界ストリップで表現) -----
    // 深さ: 水面(shallow)→深部(deep) を頂点ごとに補間。SpriteBatch::DrawTriangleVC で描く。
    void SetDepthColors(FVec3 shallow, FVec3 deep) noexcept { m_ShallowColor = shallow; m_DeepColor = deep; }
    void SetDepthAlpha(f32 shallow_alpha, f32 deep_alpha) noexcept { m_ShallowAlpha = shallow_alpha; m_DeepAlpha = deep_alpha; }
    // 陸際の白泡: width=0 で bbox から自動。speed=アニメ速度、alpha=濃さ。
    void SetFoam(FVec3 color, f32 width = 0.0f, f32 speed = 3.0f, f32 alpha = 0.9f) noexcept {
        m_FoamColor = color; m_FoamWidth = width; m_FoamSpeed = speed; m_FoamAlpha = alpha;
    }
    void EnableFoam(bool on) noexcept { m_FoamEnabled = on; }
    // 水際を引き締める縁取り (内側へフェードする細い帯)。
    void SetRim(FVec3 color, f32 width = 0.0f, f32 alpha = 0.5f) noexcept {
        m_RimColor = color; m_RimWidth = width; m_RimAlpha = alpha;
    }
    void EnableRim(bool on) noexcept { m_RimEnabled = on; }

    // 平面反射: 水面より上のシーンを鏡像で映す (FScene2D::SetReflectionEnabled(true)
    // が前提。RT が rc に配線されていなければ無視)。tint/alpha で色味と強さ。
    void SetReflection(bool enable, FVec3 tint = FVec3{1.0f, 1.0f, 1.0f}, f32 alpha = 0.55f) noexcept {
        m_ReflectEnabled = enable; m_ReflectTint = tint; m_ReflectAlpha = alpha;
    }
    void SetReflectionDistortion(f32 amplitude_mult) noexcept { m_ReflectDistort = amplitude_mult; }

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
    void Finish() noexcept;                            // weight / bbox / 境界を再計算
    void BuildBoundary() noexcept;                     // edge-incidence で外周ループ + 外向き法線
    f32  WaveAt(FVec2 world) const noexcept;           // ambient 波 (流れ方向) + 波紋
    FVec4 DepthColorAt(u32 vert_index) const noexcept; // 深さ勾配の頂点カラー

    struct Ripple { f32 x = 0, amp = 0, amp0 = 0, time = 0; bool active = false; };
    static constexpr u32 kMaxRipples = 16;
    Ripple m_Ripples[kMaxRipples];

    FVec2 m_Vert[kMaxVerts];        // owner 相対の頂点
    f32   m_Weight[kMaxVerts];      // 水面らしさ [0,1] (bbox 上端ほど 1)
    u32   m_VCount = 0;
    u16   m_Tri[kMaxTris * 3];      // m_Vert への index (3 個 1 組)
    u32   m_TCount = 0;
    u16   m_Boundary[kMaxVerts + 1]; // 外周ループ (m_Vert への順序付き index)
    u32   m_BoundaryCount = 0;
    FVec2 m_BoundaryNormal[kMaxVerts]; // 各境界頂点の外向き単位法線 (owner 相対)
    f32   m_MinX = 0, m_MaxX = 0, m_MinY = 0, m_MaxY = 0;

    // 深さ勾配 (頂点カラー)。SetColor/SetAlpha は後方互換でこれらを設定。
    FVec3 m_ShallowColor{0.20f, 0.55f, 0.78f};
    FVec3 m_DeepColor{0.04f, 0.16f, 0.32f};
    f32   m_ShallowAlpha = 0.62f;
    f32   m_DeepAlpha    = 0.92f;
    // 泡 / 縁。
    FVec3 m_FoamColor{0.95f, 0.98f, 1.0f};
    f32   m_FoamWidth = 0.0f;       // 0 = bbox から自動
    f32   m_FoamSpeed = 3.0f;
    f32   m_FoamAlpha = 0.9f;
    bool  m_FoamEnabled = true;
    FVec3 m_RimColor{0.55f, 0.80f, 0.95f};
    f32   m_RimWidth = 0.0f;        // 0 = 自動
    f32   m_RimAlpha = 0.5f;
    bool  m_RimEnabled = true;
    // 平面反射。
    bool  m_ReflectEnabled = false;
    FVec3 m_ReflectTint{1.0f, 1.0f, 1.0f};
    f32   m_ReflectAlpha = 0.55f;
    f32   m_ReflectDistort = 1.0f;

    FVec3 m_Color{0.12f, 0.35f, 0.6f};
    f32   m_Alpha = 0.72f;
    f32   m_Amp   = 0.12f;
    f32   m_Speed = 1.6f;
    f32   m_Time  = 0.0f;
    FVec2 m_FlowDir{1.0f, 0.0f};   // 波が流れる向き (正規化済)
    f32   m_FlowSpeed = 1.6f;       // 流れの速さ
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

// ===========================================================================
// FStencilClip2DComponent — 任意形状のステンシルマスクで子ツリーをクリップ
// ===========================================================================
// このコンポーネントを attach したノードの「子ツリー」を、指定形状の内側 (既定) か
// 外側だけにクリップして描く。窓越しに他のエフェクトを覗かせる / 穴あきマスク等。
// シェーダ不要 — SpriteBatch の stencil モードを使う。
//
// **前提**: シーンで `SetStencilMaskEnabled(true)` を呼んでおくこと。stencil バッファ
//           が無いパス (例: 反射の RT パス) では自動的に素通し (クリップしない)。
//
//   auto win = MakeUnique<FNode2D>();
//   win->Local().position = {0, 0};
//   auto& clip = win->AddComponent<FStencilClip2DComponent>();
//   clip.SetCircle({0, 0}, 2.5f);          // 窓の形 (owner 相対)
//   win->AddChild(MakeWaterNode());        // 子 = 窓の中だけに見えるエフェクト
//   root.AddChild(Move(win));
class FStencilClip2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FStencilClip2DComponent)

    static constexpr u32 kMaxTris = 96;

    // ----- マスク形状 (すべて owner 相対) -----
    void SetRect(FVec2 center, FVec2 half_size) noexcept;
    void SetCircle(FVec2 center, f32 radius, u32 segments = 40) noexcept;
    void SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments = 40) noexcept;
    void SetPolygon(const FVec2* pts, u32 count) noexcept;   // 単純多角形 (centroid fan)

    // 内側(false=既定) ではなく外側だけを残す (穴あき)。
    void SetInvert(bool clip_outside) noexcept { m_Outside = clip_outside; }
    // ネスト用のステンシル参照値 (1..255、既定 1)。入れ子マスクは別値にすること。
    void SetRef(u8 ref) noexcept { m_Ref = ref < 1 ? 1u : ref; }
    // マスク形状自体を可視化する (デバッグ。既定 false = 不可視)。
    void SetDebugVisualize(bool on, FVec4 color = FVec4{1.0f, 1.0f, 0.0f, 0.25f}) noexcept {
        m_DebugVis = on; m_DebugColor = color;
    }

    void OnDraw(RenderContext& rc) noexcept override;              // マスク書込み → KeepInside
    void OnDrawPostChildren(RenderContext& rc) noexcept override;  // マスク解除

private:
    void PushTri(FVec2 a, FVec2 b, FVec2 c) noexcept;

    FVec2 m_Tri[kMaxTris * 3];     // owner 相対の三角形頂点 (3 個 1 組、扇状)
    u32   m_TriCount = 0;
    bool  m_Outside  = false;
    u8    m_Ref      = 1;
    bool  m_Active   = false;      // OnDraw でマスクを張ったか (PostChildren 解除判定)
    bool  m_DebugVis = false;
    FVec4 m_DebugColor{1.0f, 1.0f, 0.0f, 0.25f};
};

} // namespace acs::game
