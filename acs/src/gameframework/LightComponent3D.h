// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Quat.h"
#include "gameframework/AComponent.h"
#include "gameframework/ANode.h"
#include "render/StandardShader.h"   // FDirLight / FPointLight

namespace acs::game {

/** 光の種類。 */
enum class ELight3DKind : u8 {
    /** 平行光源 (太陽)。位置は無視され、ノードの向きだけを使う。 */
    Directional = 0,

    /** 点光源。ノードの位置から全方向へ届く。 */
    Point       = 1,
};

/**
 * シーンに置ける 3D の光 (ALight2DComponent の 3D 版)。
 *
 * @details
 * `FDirLight` / `FPointLight` はシェーダへ渡す値そのもので、ノード階層とは結び付いていない。
 * このコンポーネントはノードに付き、**ワールド変換から向きと位置を導いて**その値を作る。
 * ノードを動かせば光も動き、親に付ければ一緒に動く。
 *
 * 向きはノードの回転を **-Y (真下)** に適用したもの。無回転なら真上から下を照らす。
 * これは `FDirLight::direction` の既定 (0, -1, 0) と揃えてある。
 *
 * @code
 * ANode& sun = root.AddChild(NewObject<ANode>());
 * ALightComponent3D& light = sun.AddComponent<ALightComponent3D>();
 * light.SetLightKind(ELight3DKind::Directional);
 * light.SetColor(FVec3{1.0f, 0.96f, 0.9f});
 * light.SetIntensity(3.0f);
 *
 * FDirLight out{};
 * if (light.FillDirectional(out)) { shader.SetLights(vp, ..., &out, 1, ...); }
 * @endcode
 */
class ALightComponent3D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ALightComponent3D)

    /** 既定値 (やや暖かい白の平行光源) で構築する。 */
    ALightComponent3D() noexcept = default;

    /**
     * 種類を指定して構築する。
     *
     * @param kind 光の種類。
     */
    explicit ALightComponent3D(ELight3DKind kind) noexcept : m_Kind(kind) {}

    /**
     * 光の種類を返す。
     *
     * @return 現在の種類。
     */
    ELight3DKind LightKind() const noexcept { return m_Kind; }

    /**
     * 光の種類を設定する。
     *
     * @param kind 設定する種類。
     */
    void SetLightKind(ELight3DKind kind) noexcept { m_Kind = kind; }

    /**
     * 光の色を返す。
     *
     * @return 線形空間の RGB。
     */
    FVec3 Color() const noexcept { return m_Color; }

    /**
     * 光の色を設定する。
     *
     * @param c 線形空間の RGB。
     */
    void SetColor(FVec3 c) noexcept { m_Color = c; }

    /**
     * 強さを返す。
     *
     * @return 色に掛ける倍率。
     */
    f32 Intensity() const noexcept { return m_Intensity; }

    /**
     * 強さを設定する (負値は 0 に丸める)。
     *
     * @param i 色に掛ける倍率。
     */
    void SetIntensity(f32 i) noexcept { m_Intensity = i > 0.0f ? i : 0.0f; }

    /**
     * 点光源の到達距離を返す。
     *
     * @return 到達距離。
     */
    f32 Range() const noexcept { return m_Range; }

    /**
     * 点光源の到達距離を設定する (0 以下は無視する)。
     *
     * @details 0 にすると光がまったく届かなくなり、消えているのと区別が付かない。
     * @param r 到達距離。
     */
    void SetRange(f32 r) noexcept { if (r > 0.0f) m_Range = r; }

    /**
     * 影を落とすかを返す。
     *
     * @return 落とすなら true。
     */
    bool CastsShadow() const noexcept { return m_CastShadow; }

    /**
     * 影を落とすかを設定する。
     *
     * @param b 落とすなら true。
     */
    void SetCastsShadow(bool b) noexcept { m_CastShadow = b; }

    /**
     * 光っているかを返す。
     *
     * @details 強さが 0 なら、計算しても何も足されない。集める側が飛ばすために使う。
     * @return 強さが正なら true。
     */
    bool IsEmitting() const noexcept { return m_Intensity > 0.0f; }

    /**
     * ノードの向きから、光が進む方向を返す。
     *
     * @details 無回転なら (0, -1, 0)。ノードに付いていない場合もその既定を返す。
     * @return ワールド空間の方向 (正規化済み)。
     */
    FVec3 WorldDirection() const noexcept {
        if (!HasOwner()) return FVec3{ 0.0f, -1.0f, 0.0f };

        return Rotate(Owner().World().rotation, FVec3{ 0.0f, -1.0f, 0.0f });
    }

    /**
     * 平行光源としての値を書き出す。
     *
     * @details 種類が違う、または光っていない場合は **out を触らずに false を返す**。
     * @param out 書き出し先。
     * @return 書き出したら true。
     */
    bool FillDirectional(FDirLight& out) const noexcept {
        if (m_Kind != ELight3DKind::Directional || !IsEmitting()) return false;

        out.direction = WorldDirection();
        out.color     = FVec3{ m_Color.x * m_Intensity,
                               m_Color.y * m_Intensity,
                               m_Color.z * m_Intensity };
        return true;
    }

    /**
     * 点光源としての値を書き出す。
     *
     * @details 種類が違う、または光っていない場合は **out を触らずに false を返す**。
     * @param out 書き出し先。
     * @return 書き出したら true。
     */
    bool FillPoint(FPointLight& out) const noexcept {
        if (m_Kind != ELight3DKind::Point || !IsEmitting()) return false;

        out.position = HasOwner() ? Owner().World().position : FVec3{ 0.0f, 0.0f, 0.0f };
        out.range    = m_Range;
        out.color    = FVec3{ m_Color.x * m_Intensity,
                              m_Color.y * m_Intensity,
                              m_Color.z * m_Intensity };
        return true;
    }

private:
    /** 光の色 (線形空間、強さは別に持つ)。 */
    FVec3 m_Color{ 1.0f, 0.96f, 0.9f };

    /** 色に掛ける倍率。 */
    f32 m_Intensity = 1.0f;

    /** 点光源の到達距離。 */
    f32 m_Range = 10.0f;

    /** 光の種類。 */
    ELight3DKind m_Kind = ELight3DKind::Directional;

    /** 影を落とすか。 */
    bool m_CastShadow = true;
};

} // namespace acs::game
