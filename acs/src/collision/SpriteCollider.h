// SPDX-License-Identifier: Apache-2.0
// 2D スプライトコライダー — スプライトのアルファチャンネルから形状に沿った
// コライダー (凸包 + 簡略化済み輪郭ポリゴン) を生成する。
//
//   acs::FSpriteCollider col;
//   col.BuildFromAlpha(rgba, w, h, /*alpha_threshold=*/128, /*simplify=*/1.5f);
//   col.ContainsPoint({px, py});      // 輪郭ポリゴンの内外判定
//   col.Hull(); col.HullCount();      // 凸包 (物理で扱いやすい・常に有効)
//   col.Outline(); col.OutlineCount();// 簡略化された輪郭 (凹形状にも追従)
//   col.Bounds();                     // AABB
//
// 凸包は Jarvis march (順序非依存で堅牢)、輪郭は Moore 近傍トレース + Douglas-Peucker
// 簡略化。座標はピクセル空間 (左上原点)。単一連結成分・穴なしを前提とする。
//
// ACS 規約: STL/<string> 不使用、全 noexcept、TResult。視覚非依存の純 CPU ロジック
// なのでヘッドレスで完全検証できる。
#pragma once

#include "foundation/Result.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

namespace acs {

class FSpriteCollider {
public:
    static constexpr u32 kMaxVertices = 256;

    FSpriteCollider() noexcept = default;
    ~FSpriteCollider() noexcept = default;
    FSpriteCollider(const FSpriteCollider&)            = delete;
    FSpriteCollider& operator=(const FSpriteCollider&) = delete;

    // rgba: 上から下・左から右の RGBA8 tightly-packed。alpha >= threshold を「内側」と判定。
    // simplify_epsilon: 輪郭の Douglas-Peucker 許容誤差 (px)。大きいほど頂点が減る。
    TResult<void> BuildFromAlpha(const acs::u8* rgba, acs::u32 width, acs::u32 height,
                                 acs::u8 alpha_threshold = 128,
                                 acs::f32 simplify_epsilon = 1.5f) noexcept;
    void Clear() noexcept;

    const FVec2* Hull()        const noexcept { return m_Hull; }
    acs::u32     HullCount()   const noexcept { return m_HullCount; }
    const FVec2* Outline()     const noexcept { return m_Outline; }
    acs::u32     OutlineCount() const noexcept { return m_OutlineCount; }
    Aabb2        Bounds()      const noexcept { return m_Bounds; }

    // 簡略化済み輪郭ポリゴンの内外判定 (ray-crossing、凹形状対応)。
    bool ContainsPoint(FVec2 p) const noexcept;

    // 凸包を ConvexPoly2 (物理用、最大 16 頂点) に変換。FCollisionWorld2D::AddPolygon
    // にそのまま渡せる。頂点が多い場合は均等に間引く。
    ConvexPoly2 HullPolygon() const noexcept;

private:
    FVec2     m_Hull[kMaxVertices]{};
    acs::u32  m_HullCount = 0;
    FVec2     m_Outline[kMaxVertices]{};
    acs::u32  m_OutlineCount = 0;
    Aabb2     m_Bounds{};
};

} // namespace acs
