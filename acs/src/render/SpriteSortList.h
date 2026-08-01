// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "render/DrawPacketSortKey.h"

namespace acs {

class IRhiTexture;   // 前方宣言 (コマンドはポインタのみ保持)
class CSpriteBatch;  // Replay の対象 (定義は .cpp 側)

/**
 * ソートリストに積む 1 コマンドの種別。
 */
enum class ESpriteCmdKind : u8 {
    /** テクスチャ付きスプライト (UV 部分 + tint)。 */
    Textured,

    /** 単色矩形 (テクスチャ無し)。 */
    Rect,
};

/**
 * ソート可能な 1 描画コマンド (テクスチャ付きスプライト or 単色矩形)。
 *
 * @details layer/depth がソートキー、seq は同キー時の安定 tie-break (挿入順)。
 */
struct FSpriteCmd {
    /** コマンド種別。 */
    ESpriteCmdKind kind  = ESpriteCmdKind::Rect;

    /** 第1ソートキー (昇順: 小さい層が奥)。 */
    i32   layer = 0;

    /** 第2ソートキー (昇順: 小さい depth が奥)。 */
    f32   depth = 0.0f;

    /** 左上 X (ピクセル)。 */
    f32   x = 0.0f;

    /** 左上 Y (ピクセル)。 */
    f32   y = 0.0f;

    /** 幅 (ピクセル)。 */
    f32   w = 0.0f;

    /** 高さ (ピクセル)。 */
    f32   h = 0.0f;

    /** サンプル領域の左 U (Textured のみ)。 */
    f32   u0 = 0.0f;

    /** サンプル領域の上 V (Textured のみ)。 */
    f32   v0 = 0.0f;

    /** サンプル領域の右 U (Textured のみ)。 */
    f32   u1 = 1.0f;

    /** サンプル領域の下 V (Textured のみ)。 */
    f32   v1 = 1.0f;

    /** Rect=塗り色 / Textured=乗算 tint。 */
    FVec4 color{ 1, 1, 1, 1 };

    /** 貼り付けるテクスチャ (Textured のみ、Rect は nullptr)。 */
    IRhiTexture* tex = nullptr;

    /** 安定 tie-break 用の挿入シーケンス番号。 */
    u32   seq = 0;
};

/**
 * CSpriteBatch 用の明示的 depth/layer 順序レイヤ (opt-in)。
 *
 * @details
 * 描画コマンドを layer/depth 付きで貯め、安定ソートして CSpriteBatch に sorted 順で流す。
 * CSpriteBatch は無改変のまま「提出順」を保つので、本クラスを使わない経路は一切変わらない。
 * 非コピー (コマンド配列を単独所有)。
 */
class FSpriteSortList {
public:
    /** 空状態で構築する。 */
    FSpriteSortList() noexcept = default;

    /** 破棄する。 */
    ~FSpriteSortList() noexcept = default;

    /** コピー禁止。 */
    FSpriteSortList(const FSpriteSortList&)            = delete;
    FSpriteSortList& operator=(const FSpriteSortList&) = delete;

    /**
     * コマンドバッファを空にする (毎フレーム先頭で呼ぶ)。
     *
     * @details seq カウンタも 0 に戻す。
     */
    void Clear() noexcept {
        m_Cmds.Clear();
        m_Order.Clear();
        m_Scratch.Clear();
        m_Keys.Clear();
        m_Seq = 0;
        m_LastSortPasses = 0u;
        m_LastSortItemVisits = 0u;
    }

    /**
     * コマンド配列の容量を予約する。
     *
     * @param n 予約するコマンド数。
     */
    void Reserve(u32 n) noexcept {
        m_Cmds.Reserve(n);
        m_Order.Reserve(n);
        m_Scratch.Reserve(n);
        m_Keys.Reserve(n);
    }

    /**
     * 積まれたコマンド数を返す。
     *
     * @return コマンド総数。
     */
    u32 Count() const noexcept { return static_cast<u32>(m_Cmds.Size()); }

    /**
     * テクスチャ全体のスプライトを積む。
     *
     * @param tex 描画テクスチャ。
     * @param x 左上 X (px)。
     * @param y 左上 Y (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @param layer ソート層 (昇順: 小=奥)。
     * @param depth 層内 depth (昇順: 小=奥)。
     * @param tint 乗算色 (既定は白)。
     */
    void Submit(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h,
                i32 layer, f32 depth, FVec4 tint = FVec4{1, 1, 1, 1}) noexcept {
        SubmitSub(tex, x, y, w, h, 0, 0, 1, 1, layer, depth, tint);
    }

    /**
     * テクスチャの一部 (UV 0..1) のスプライトを積む。
     *
     * @param tex 描画テクスチャ。
     * @param x 左上 X (px)。
     * @param y 左上 Y (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @param u0 左 U。
     * @param v0 上 V。
     * @param u1 右 U。
     * @param v1 下 V。
     * @param layer ソート層 (昇順: 小=奥)。
     * @param depth 層内 depth (昇順: 小=奥)。
     * @param tint 乗算色 (既定は白)。
     */
    void SubmitSub(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   i32 layer, f32 depth, FVec4 tint = FVec4{1, 1, 1, 1}) noexcept {
        FSpriteCmd c;
        c.kind  = ESpriteCmdKind::Textured;
        c.layer = layer; c.depth = depth;
        c.x = x; c.y = y; c.w = w; c.h = h;
        c.u0 = u0; c.v0 = v0; c.u1 = u1; c.v1 = v1;
        c.color = tint;
        c.tex = &tex;
        c.seq = m_Seq++;
        m_Cmds.PushBack(c);
    }

    /**
     * 単色矩形を積む。
     *
     * @param x 左上 X (px)。
     * @param y 左上 Y (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @param color 塗りつぶし色。
     * @param layer ソート層 (昇順: 小=奥)。
     * @param depth 層内 depth (昇順: 小=奥)。
     */
    void SubmitRect(f32 x, f32 y, f32 w, f32 h, FVec4 color,
                    i32 layer, f32 depth) noexcept {
        FSpriteCmd c;
        c.kind  = ESpriteCmdKind::Rect;
        c.layer = layer; c.depth = depth;
        c.x = x; c.y = y; c.w = w; c.h = h;
        c.color = color;
        c.tex = nullptr;
        c.seq = m_Seq++;
        m_Cmds.PushBack(c);
    }

    /**
     * (layer 昇順, depth 昇順, seq 昇順) で安定ソートしてソート順を確定する。
     *
     * @details compile-time layoutでlayer/depthを64bit keyへ変換し、小規模は挿入、
     * 大規模は安定LSD radixで並べる。同一keyは提出順を保持する。
     */
    void Sort() noexcept;

    /** 直前のSortで実行したradix byte pass数を返す。 */
    u32 LastSortPassCount() const noexcept { return m_LastSortPasses; }

    /** 直前のSortでkey生成、比較、radix走査したitem数を返す。 */
    u64 LastSortItemVisits() const noexcept { return m_LastSortItemVisits; }

    /**
     * ソート済み順で i 番目のコマンドへの const 参照を返す (検証用)。
     *
     * @details Sort() 前は提出順、Sort() 後はソート順。範囲外は未定義 (呼び出し側責務)。
     * @param i ソート順インデックス。
     * @return i 番目のコマンド。
     */
    const FSpriteCmd& Ordered(u32 i) const noexcept {
        // Sort() 済み (m_Order が全コマンドを覆う) ならソート順、未ソートなら提出順。
        return (m_Order.Size() == m_Cmds.Size()) ? m_Cmds[m_Order[i]] : m_Cmds[i];
    }

    /**
     * ソート済み順に全コマンドを CSpriteBatch へ流す。
     *
     * @details
     * Sort() 済みであることが前提 (未ソートなら提出順で流す)。Begin/End は呼び出し側で行う。
     * Textured は DrawSub、Rect は DrawRect を呼ぶ。
     * @param sb 描画先の (Begin 済み) スプライトバッチ。
     */
    void Replay(CSpriteBatch& sb) const noexcept;

private:
    /** 積まれたコマンド (提出順、ソートで動かさない)。 */
    TArray<FSpriteCmd> m_Cmds;

    /** ソート結果のインデックス順 (Sort で確定、Clear/Submit 後は Sort まで未確定)。 */
    TArray<u32>        m_Order;

    /** radix passの書き込み先として再利用するindex列。 */
    TArray<u32>        m_Scratch;

    /** command本体を動かさず再利用する64bit sort key列。 */
    TArray<u64>        m_Keys;

    /** 次に振る挿入シーケンス番号。 */
    u32                m_Seq = 0;

    /** 直前に実行したradix byte pass数。 */
    u32                m_LastSortPasses = 0u;

    /** 直前のSortが走査したitem数。 */
    u64                m_LastSortItemVisits = 0u;
};

} // namespace acs
