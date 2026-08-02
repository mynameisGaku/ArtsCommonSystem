// SPDX-License-Identifier: Apache-2.0
// GameFramework — APolygonRenderer2D (任意凸ポリゴンの塗りつぶし描画コンポーネント)
//
// APrimitiveRenderer2D が Box/Circle/Triangle のみなのに対し、本コンポーネントは
// «ローカル頂点列» を持ち owner の world transform を適用して三角形ファンで塗る。
// エディタのポリゴンノード (ACSCENE の POLY 行) をスタンドアロンで再現するのに使う。
// 凹ポリゴンは fan が破綻し得る (best-effort、凸前提)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"
#include "gameframework/AComponent.h"
#include "gameframework/ANode.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"

namespace acs::game {

/**
 * ローカル頂点列を owner の world transform で変換して塗りつぶす凸ポリゴン描画コンポーネント。
 */
class APolygonRenderer2D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(APolygonRenderer2D)

    /** 保持できる頂点数の上限。 */
    static constexpr u32 kMaxVerts = 64u;

    /** 塗り色を設定する。 */
    void SetColor(FVec4 c) noexcept { m_Color = c; }

    /**
     * ローカル頂点列 (node 原点基準) を設定する。
     *
     * @param v 頂点配列。
     * @param count 頂点数 (kMaxVerts で頭打ち)。
     */
    void SetVerts(const FVec2* v, u32 count) noexcept {
        m_Count = count < kMaxVerts ? count : kMaxVerts;
        for (u32 i = 0; i < m_Count; ++i) m_Verts[i] = v[i];
    }

    /** 頂点数。 */
    u32 VertCount() const noexcept { return m_Count; }

    /** 指定インデックスのローカル頂点 (境界外は原点)。 */
    FVec2 Vert(u32 i) const noexcept { return (i < m_Count) ? m_Verts[i] : FVec2{ 0.0f, 0.0f }; }

    /** owner の world transform でローカル頂点を変換し、三角形ファンで塗る。 */
    void OnDraw(FRenderContext& rc) noexcept override {
        if (!rc.HasSprites() || m_Count < 3u || !HasOwner()) return;
        const FTransform2D wt = Owner().World2D();
        const f32 c = Cos(wt.rotation), s = Sin(wt.rotation);
        FVec2 w[kMaxVerts];
        for (u32 i = 0; i < m_Count; ++i) {
            const f32 lx = m_Verts[i].x * wt.scale.x;
            const f32 ly = m_Verts[i].y * wt.scale.y;
            w[i] = FVec2{ wt.position.x + lx * c - ly * s, wt.position.y + lx * s + ly * c };
        }
        CSpriteBatch& sb = rc.Sprites();
        for (u32 i = 1u; i + 1u < m_Count; ++i)   // vert 0 を扇の中心に三角形ファン
            sb.DrawTriangle(w[0].x, w[0].y, w[i].x, w[i].y, w[i + 1u].x, w[i + 1u].y, m_Color);
    }

private:
    /** ローカル頂点 (node 原点基準)。 */
    FVec2 m_Verts[kMaxVerts]{};

    /** 頂点数。 */
    u32   m_Count = 0u;

    /** 塗り色。 */
    FVec4 m_Color{ 0.6f, 0.7f, 0.9f, 1.0f };
};

} // namespace acs::game
