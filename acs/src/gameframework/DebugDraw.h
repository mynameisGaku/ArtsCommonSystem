// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugDraw (immediate-mode デバッグ図形バッファ)
//
// 1 フレーム分の線分プリミティブを蓄積するだけのバッファ。実描画は行わない。
//
// 設計選択:
//   ・**描画と分離**: FDebugDraw 自体はジオメトリ蓄積のみ。レンダラ非依存。
//     描画システムが Lines() / LineCount() を読み取って FSpriteBatch 等で
//     1 フレーム末にまとめて描画する想定。Pillar H の他の描画系（FSpriteBatch /
//     particle system / 自前 DX12）どれでも消費できる。
//   ・**immediate-mode API**: DrawLine / DrawAabb / DrawCircle / DrawCross を
//     ゲームロジックの任意の場所から呼べる。フレーム頭で Clear() を呼ぶだけ。
//     state を持たず、衝突判定や物理デバッグから手軽に書ける。
//   ・**テストしやすい**: 描画副作用がないので、unit test では蓄積された
//     FLine 配列をそのまま検査できる（座標が正しいか・本数が合うか等）。
//   ・**ゼロ寄与パス可**: Clear() しなければ蓄積され続け、Clear() を毎フレーム
//     呼べば「今フレームの形状」だけが残る。描画システム側が消費後に Clear する
//     運用も可能（呼び出し責任は user に委ねる、library 側は意見を持たない）。
//   ・**Circle は segment 化**: 内部で線分列に分解して保持。描画側はライン
//     ジオメトリだけ扱えば OK（円専用パスを描画側に要求しない）。
//   ・**非コピー・非ムーブ**: 内部 TArray が大きくなりがちで、誤コピー事故を防ぐ。
//     ゲーム全体で 1 インスタンス（Services 経由か static）を想定。
//
// 使い方:
//   acs::game::FDebugDraw dd;
//   void Frame() {
//       dd.Clear();
//       dd.DrawAabb(player_aabb, FVec4{1,0,0,1});
//       dd.DrawCircle(enemy_circle, FVec4{1,1,0,1});
//       // 描画システム:
//       for (u32 i = 0; i < dd.LineCount(); ++i) {
//           const auto& ln = dd.Lines()[i];
//           sprite_batch.DrawLine(ln.a, ln.b, ln.color);
//       }
//   }
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Collision2D.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 1 フレーム分の線分プリミティブを蓄積する immediate-mode デバッグ図形バッファ。
 *
 * @details
 * 実描画は行わず、ジオメトリの蓄積のみを担うレンダラ非依存の state holder。
 * DrawLine/DrawAabb/DrawCircle/DrawCross をゲームロジックの任意の場所から呼び、
 * フレーム頭で Clear() する運用を想定する。描画システムは Lines() / LineCount()
 * を読み取って FSpriteBatch 等でまとめて描画する。円は内部で線分列に分解して保持する。
 * 内部 TArray の誤コピー事故を防ぐため非コピー・非ムーブ。
 */
class FDebugDraw {
public:
    /**
     * 描画システムが読み取る生バッファ要素 (ライン 1 本)。
     *
     * @details 2 端点と色を持つ。Circle 等も最終的にこの FLine 列へ分解される。
     */
    struct FLine {
        /** 線分の始点。 */
        FVec2 a;

        /** 線分の終点。 */
        FVec2 b;

        /** 線分の RGBA 色。 */
        FVec4 color;
    };

    /** 空のバッファを構築する。 */
    FDebugDraw() noexcept = default;

    /** 破棄する (内部 TArray が解放)。 */
    ~FDebugDraw() noexcept = default;

    /** コピー禁止 (内部 TArray の誤コピーを防ぐため)。 */
    FDebugDraw(const FDebugDraw&)            = delete;

    /** コピー代入も禁止。 */
    FDebugDraw& operator=(const FDebugDraw&) = delete;

    /** ムーブ禁止 (所有移譲事故を防ぐため)。 */
    FDebugDraw(FDebugDraw&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FDebugDraw& operator=(FDebugDraw&&)      = delete;

    /**
     * 任意 2 点間の線分を 1 本蓄積する。
     *
     * @param a 線分の始点。
     * @param b 線分の終点。
     * @param color 線分の RGBA 色。
     */
    void DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept;

    /**
     * AABB の輪郭を 4 辺の線分として蓄積する (中身は塗らない)。
     *
     * @param a 輪郭を描く軸並行境界ボックス。
     * @param color 線分の RGBA 色。
     */
    void DrawAabb(const FAabb2& a, FVec4 color) noexcept;

    /**
     * 円を segments 本の線分に分解した近似輪郭を蓄積する。
     *
     * @details segments < 3 のときは 3 (三角形) に丸めて縮退を防ぐ。24 で十分滑らか。
     * @param c 輪郭を描く円 (中心と半径)。
     * @param color 線分の RGBA 色。
     * @param segments 分割数 (既定 24)。
     */
    void DrawCircle(const FCircle& c, FVec4 color, u32 segments = 24) noexcept;

    /**
     * 中心 pos の "+" 記号 (横線 + 縦線) を蓄積する。位置の可視化に使う。
     *
     * @param pos "+" の中心座標。
     * @param size 中心から各方向への片側長。
     * @param color 線分の RGBA 色。
     */
    void DrawCross(FVec2 pos, f32 size, FVec4 color) noexcept;

    /**
     * a→b の矢印 (軸 1 本 + 矢じり 2 本 = 計 3 線) を蓄積する。速度/法線ベクトルの可視化に使う。
     *
     * @param a 矢印の始点。
     * @param b 矢印の終点 (矢じりが付く)。
     * @param color 線分の RGBA 色。
     * @param head_len 矢じりの長さ (0 で軸長の 20%)。
     */
    void DrawArrow(FVec2 a, FVec2 b, FVec4 color, f32 head_len = 0.0f) noexcept;

    /** 蓄積した線分をクリアする (容量は保持)。フレーム頭か描画消費後に呼ぶ。 */
    void Clear() noexcept { m_Lines.Clear(); }

    /**
     * 蓄積されている線分の本数を返す。
     *
     * @return 蓄積線数。
     */
    u32 LineCount() const noexcept { return static_cast<u32>(m_Lines.Size()); }

    /**
     * 描画システムが読み取る生バッファ先頭を返す (連続メモリ保証)。
     *
     * @return 線分配列の先頭ポインタ。空のときは nullptr (利用側は LineCount() でガードすること)。
     */
    const FLine* Lines() const noexcept {
        return m_Lines.IsEmpty() ? nullptr : &m_Lines.begin()[0];
    }

private:
    /** 蓄積中の線分バッファ。 */
    TArray<FLine> m_Lines;
};

} // namespace acs::game
