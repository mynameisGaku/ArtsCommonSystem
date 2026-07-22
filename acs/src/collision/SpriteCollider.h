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

/**
 * スプライトのアルファチャンネルから形状に沿ったコライダーを生成する 2D コライダー。
 *
 * @details
 * 不透明画素の境界から凸包 (Jarvis march、順序非依存で堅牢) と簡略化済み輪郭ポリゴン
 * (Moore 近傍トレース + Douglas-Peucker 簡略化) を作る。凸包は物理で扱いやすく常に有効、
 * 輪郭は凹形状にも追従する。座標はピクセル空間 (左上原点) で、単一連結成分・穴なしを前提。
 * ACS 規約に従い non-copy・全 noexcept・TResult を用いる純 CPU ロジック。
 */
class FSpriteCollider {
public:
    /** 凸包・輪郭それぞれに保持する頂点数の上限。 */
    static constexpr u32 kMaxVertices = 256;

    /** 空のコライダーを構築する (形状は BuildFromAlpha で作る)。 */
    FSpriteCollider() noexcept = default;

    /** 破棄する (頂点は固定長配列のため特別な解放は不要)。 */
    ~FSpriteCollider() noexcept = default;

    /** コピー禁止。 */
    FSpriteCollider(const FSpriteCollider&)            = delete;

    /** コピー代入も禁止。 */
    FSpriteCollider& operator=(const FSpriteCollider&) = delete;

    /**
     * RGBA8 画像のアルファから凸包・輪郭・AABB を構築する。
     *
     * @details
     * alpha >= alpha_threshold を「内側 (不透明)」と判定し、不透明かつ 4 近傍に背景がある
     * 境界画素を集める。凸包は Jarvis march、輪郭は Moore 近傍トレース後に Douglas-Peucker で
     * 簡略化する。輪郭が取れない場合は凸包で代用する。境界画素が 3 点未満などはエラー。
     * @param rgba 上から下・左から右に詰めた RGBA8 tightly-packed 画素列。
     * @param width 画像の幅 (px)。
     * @param height 画像の高さ (px)。
     * @param alpha_threshold 「内側」とみなすアルファ下限値 (既定 128)。
     * @param simplify_epsilon 輪郭の Douglas-Peucker 許容誤差 (px)。大きいほど頂点が減る。
     * @return 成功なら空の TResult、画像が null/空・境界画素不足ならエラー。
     */
    TResult<void> BuildFromAlpha(const acs::u8* rgba, acs::u32 width, acs::u32 height,
                                 acs::u8 alpha_threshold = 128,
                                 acs::f32 simplify_epsilon = 1.5f) noexcept;

    /** 凸包・輪郭・境界を破棄して空状態に戻す。 */
    void Clear() noexcept;

    /**
     * 凸包頂点配列の先頭を返す。
     *
     * @return 凸包頂点配列 (HullCount 個有効)。
     */
    const FVec2* Hull()        const noexcept { return m_Hull; }

    /**
     * 凸包の頂点数を返す。
     *
     * @return 凸包頂点数。
     */
    acs::u32     HullCount()   const noexcept { return m_HullCount; }

    /**
     * 簡略化済み輪郭ポリゴンの頂点配列の先頭を返す。
     *
     * @return 輪郭頂点配列 (OutlineCount 個有効)。
     */
    const FVec2* Outline()     const noexcept { return m_Outline; }

    /**
     * 輪郭ポリゴンの頂点数を返す。
     *
     * @return 輪郭頂点数。
     */
    acs::u32     OutlineCount() const noexcept { return m_OutlineCount; }

    /**
     * 不透明領域を包む AABB を返す。
     *
     * @return 境界画素から求めた AABB。
     */
    FAabb2        Bounds()      const noexcept { return m_Bounds; }

    /**
     * 点が簡略化済み輪郭ポリゴンの内側かを判定する (ray-crossing、凹形状対応)。
     *
     * @param p 判定するピクセル座標の点。
     * @return 輪郭の内側なら true。
     */
    bool ContainsPoint(FVec2 p) const noexcept;

    /**
     * 凸包を ConvexPoly2 (物理用) に変換する。
     *
     * @details
     * FCollisionWorld2D::AddPolygon にそのまま渡せる。頂点が ConvexPoly2 の上限を超える
     * 場合は均等に間引く。
     * @return 凸包を表す ConvexPoly2。
     */
    FConvexPoly2 HullPolygon() const noexcept;

private:
    /** 凸包の頂点 (反時計回り、HullCount 個有効)。 */
    FVec2     m_Hull[kMaxVertices]{};

    /** 凸包の頂点数。 */
    acs::u32  m_HullCount = 0;

    /** 簡略化済み輪郭ポリゴンの頂点 (OutlineCount 個有効)。 */
    FVec2     m_Outline[kMaxVertices]{};

    /** 輪郭ポリゴンの頂点数。 */
    acs::u32  m_OutlineCount = 0;

    /** 不透明領域を包む AABB。 */
    FAabb2     m_Bounds{};
};

} // namespace acs
