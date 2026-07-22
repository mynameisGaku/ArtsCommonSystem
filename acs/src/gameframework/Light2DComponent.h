// SPDX-License-Identifier: Apache-2.0
// ALight2DComponent — ノードを 2D 点光源にするコンポーネント。
//
// ノードに付けると、その world 位置を光源として lit スプライト (PBR マテリアル) を
// 照らす。半径・色・強度を持つ。エディタでは Add Component で付けて gizmo で動かせる。
// 値はリフレクション (ACS_RPROP) でエディタが保持し、DrawScene がライトを集めて
// FSpriteBatch::SetLights に渡す。
#pragma once

#include "gameframework/AComponent.h"
#include "math/Vec.h"

#include <cmath>

namespace acs::game {

/** ライトの角度(度)+円錐角(度)から方向ベクトルと円錐 cos を計算する (エディタ/ランタイム共通)。 */
inline void ComputeLightDirCone(f32 angle_deg, f32 cone_deg,
                                FVec2& dir, f32& cone_inner, f32& cone_outer) noexcept {
    const f32 a = angle_deg * 0.01745329252f;   // deg→rad
    dir = FVec2{ std::cos(a), std::sin(a) };
    const f32 half = cone_deg * 0.01745329252f;
    cone_outer = std::cos(half);
    cone_inner = std::cos(half * 0.6f);          // 内側はやや細く
}

/**
 * ノードを 2D 点光源にするコンポーネント。
 *
 * @details
 * 光源位置は owner ノードの world 位置。radius (届く範囲 px)、color (RGB)、intensity
 * (明るさ) を持つ。lit スプライト (PBR マテリアル) の陰影付けに使う。エディタ DrawScene が
 * このコンポーネントを持つノードからライトを収集する。
 */
class ALight2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ALight2DComponent)

    /** 届く半径 (px)。 */
    f32   m_Radius = 256.0f;

    /** 光の色 (RGB)。 */
    FVec3 m_Color{ 1.0f, 0.95f, 0.85f };

    /** 明るさ倍率。 */
    f32   m_Intensity = 1.5f;

    /** 種類 (0=Point, 1=Spot, 2=Directional)。 */
    f32   m_Type = 0.0f;

    /** 方向角 (度、Spot/Directional)。0=+X, 90=+Y。 */
    f32   m_Angle = 90.0f;

    /** スポット円錐の半角 (度、Spot)。 */
    f32   m_ConeAngle = 35.0f;

    /** 半径を設定する。 */
    void SetRadius(f32 r) noexcept { m_Radius = r; }

    /** 色を設定する。 */
    void SetColor(FVec3 c) noexcept { m_Color = c; }

    /** 強度を設定する。 */
    void SetIntensity(f32 i) noexcept { m_Intensity = i; }

    /** 種類を設定する (ELightType の整数値)。 */
    void SetType(i32 t) noexcept { m_Type = static_cast<f32>(t); }

    /** シーンのライト収集用: 種類/半径/色/強度/方向/円錐を返す。 */
    bool QueryLight(FLightDesc2D& out) const noexcept override {
        out.type      = static_cast<i32>(m_Type);
        out.radius    = m_Radius;
        out.color     = m_Color;
        out.intensity = m_Intensity;
        ComputeLightDirCone(m_Angle, m_ConeAngle, out.dir, out.coneInner, out.coneOuter);
        return true;
    }
};

/**
 * ノードを «影を落とすもの (occluder)» にするタグコンポーネント。
 *
 * @details
 * これを付けたノードのシルエット (円近似) が lit スプライトに対してソフト影を落とす。
 * radiusScale で占有半径 (ノードサイズ比) を微調整できる。床/背景には付けない (受け手)。
 */
class AShadowCaster2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AShadowCaster2DComponent)

    /** 影の占有半径スケール (ノード半径に対する倍率)。 */
    f32 m_RadiusScale = 1.0f;

    /** 影の形状 (0=円, 1=箱)。 */
    f32 m_Shape = 0.0f;

    /** 自分自身にも影を落とすか (false=自己影スキップ・既定)。
     *  スプライトは «カメラ側を向いた面» なので通常はスキップが自然だが、
     *  厳密な 2D 平面世界として自分の落ち影で自分を暗くしたい場合に有効にする。 */
    bool m_SelfShadow = false;

    /** シーンの影オクルーダー収集用: 占有半径スケール・形状・自己影の有無を返す。 */
    bool QueryShadowCaster(f32& radius_scale, i32& shape, bool& self_shadow) const noexcept override {
        radius_scale = m_RadiusScale; shape = static_cast<i32>(m_Shape);
        self_shadow = m_SelfShadow;
        return true;
    }
};

} // namespace acs::game
