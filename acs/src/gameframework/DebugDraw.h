// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "math/Collision2D.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 1フレーム分の線分を蓄積するデバッグ図形バッファ。
 *
 * @details
 * 実描画は行わず、描画側がLines()とLineCount()から読む線分だけを保持する。
 * 円などは線分列へ分解し、内部配列の所有権事故を防ぐためコピーとムーブを禁止する。
 */
class CDebugDraw {
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
    CDebugDraw() noexcept = default;

    /** 破棄する (内部 TArray が解放)。 */
    ~CDebugDraw() noexcept = default;

    /** コピー禁止 (内部 TArray の誤コピーを防ぐため)。 */
    CDebugDraw(const CDebugDraw&)            = delete;

    /** コピー代入も禁止。 */
    CDebugDraw& operator=(const CDebugDraw&) = delete;

    /** ムーブ禁止 (所有移譲事故を防ぐため)。 */
    CDebugDraw(CDebugDraw&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDebugDraw& operator=(CDebugDraw&&)      = delete;

    /**
     * 任意2点間の線分を一括追加する。
     *
     * @param a 線分の始点。
     * @param b 線分の終点。
     * @param color 線分のRGBA色。
     * @return 成功ならtrue。有限でない入力、線分件数の範囲超過、確保失敗なら何も変更せずfalse。
     */
    bool TryDrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept;

    /**
     * 任意 2 点間の線分を 1 本蓄積する。
     *
     * @param a 線分の始点。
     * @param b 線分の終点。
     * @param color 線分の RGBA 色。
     */
    void DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept;

    /**
     * AABBの4辺を一括追加する。
     *
     * @param a 輪郭を描く軸並行境界ボックス。
     * @param color 線分のRGBA色。
     * @return 成功ならtrue。有限でない入力、座標計算や線分件数の範囲超過、確保失敗なら何も変更せずfalse。
     */
    bool TryDrawAabb(const FAabb2& a, FVec4 color) noexcept;

    /**
     * AABB の輪郭を 4 辺の線分として蓄積する (中身は塗らない)。
     *
     * @param a 輪郭を描く軸並行境界ボックス。
     * @param color 線分の RGBA 色。
     */
    void DrawAabb(const FAabb2& a, FVec4 color) noexcept;

    /**
     * 円を線分列へ分解して一括追加する。
     *
     * @details segmentsが3未満なら3へ補正する。
     * @param c 輪郭を描く円。
     * @param color 線分のRGBA色。
     * @param segments 分割数。
     * @return 成功ならtrue。有限でない入力、座標計算や線分件数の範囲超過、確保失敗なら何も変更せずfalse。
     */
    bool TryDrawCircle(const FCircle& c, FVec4 color, u32 segments = 24u) noexcept;

    /**
     * 円を segments 本の線分に分解した近似輪郭を蓄積する。
     *
     * @details segments < 3 のときは3へ丸める。失敗時は何も変更しない。
     * @param c 輪郭を描く円 (中心と半径)。
     * @param color 線分の RGBA 色。
     * @param segments 分割数 (既定 24)。
     */
    void DrawCircle(const FCircle& c, FVec4 color, u32 segments = 24) noexcept;

    /**
     * 中心posの十字を2本の線として一括追加する。
     *
     * @param pos 十字の中心座標。
     * @param size 中心から各方向への片側長。
     * @param color 線分のRGBA色。
     * @return 成功ならtrue。有限でない入力、座標計算や線分件数の範囲超過、確保失敗なら何も変更せずfalse。
     */
    bool TryDrawCross(FVec2 pos, f32 size, FVec4 color) noexcept;

    /**
     * 中心 pos の "+" 記号 (横線 + 縦線) を蓄積する。位置の可視化に使う。
     *
     * @param pos "+" の中心座標。
     * @param size 中心から各方向への片側長。
     * @param color 線分の RGBA 色。
     */
    void DrawCross(FVec2 pos, f32 size, FVec4 color) noexcept;

    /**
     * aからbへの矢印を軸と矢じりへ分解して一括追加する。
     *
     * @param a 矢印の始点。
     * @param b 矢印の終点。
     * @param color 線分のRGBA色。
     * @param head_len 矢じりの長さ。0以下なら軸長の20%。
     * @return 成功ならtrue。有限でない入力、座標計算や線分件数の範囲超過、確保失敗なら何も変更せずfalse。
     */
    bool TryDrawArrow(FVec2 a, FVec2 b, FVec4 color, f32 head_len = 0.0f) noexcept;

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
    void Clear() noexcept { m_Lines.Reset(); }

    /**
     * 蓄積されている線分の本数を返す。
     *
     * @return 蓄積線数。
     */
    u32 LineCount() const noexcept { return static_cast<u32>(m_Lines.Num()); }

    /**
     * 描画システムが読み取る生バッファ先頭を返す (連続メモリ保証)。
     *
     * @return 線分配列の先頭ポインタ。空のときは nullptr (利用側は LineCount() でガードすること)。
     */
    const FLine* Lines() const noexcept {
        return m_Lines.IsEmpty() ? nullptr : &m_Lines.begin()[0];
    }

private:
    /** 指定本数の追記領域を一括確保し、成功時だけ要素数を増やす。 */
    bool TryAppendSpace(usize count, FLine*& output) noexcept;

    /** 蓄積中の線分バッファ。 */
    TArray<FLine> m_Lines;
};

} // namespace acs::game
