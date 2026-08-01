// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (puzzle / match-3) — CMatchGrid
//
// `Bejeweled` / `Candy Crush` 系 match-3 パズルのコアロジックを担う 2D グリッド。
// セルは「色 (1..color_count)」「special 種別 (Normal / Bomb / Lightning /
// Rainbow)」「空フラグ」の 3 値を持ち、以下のサイクルを内包する:
//   1) 隣接 swap (TrySwap) → match が発生したら確定、無ければ戻す。
//   2) 3 個以上の縦横連続を検出 (DetectMatches) → FMatchInfo の列を吐く。
//   3) ResolveAllMatches でマッチを順次消化:
//        - ESpecialKind 効果 (Bomb=3x3 / Lightning=行+列 / Rainbow=同色全消し) を適用
//        - 空になった cell に対し ApplyGravity / RefillFromTop で重力 + 補充
//        - 再度マッチが発生するか走査 (cascade) → stable まで反復
//      返却値は除去総数。chain level も内部カウンタで保持し連鎖判定に使える。
//
// 使い方:
//   CMatchGrid g;
//   g.Init(8, 8, /*color_count=*/5);
//   g.FillRandom(0xC0FFEEULL);            // 初期マッチが残らない決定論埋め
//   g.SetOnClearCallback(&MyClearFx, &fx); // VFX / SFX hook
//   if (g.TrySwap(3, 4, 4, 4)) {           // 隣接 swap → 成立すれば自動 resolve
//       const u32 chains = g.ChainLevel();
//       const u32 cleared = g.TotalClearedThisChain();
//       // score 加算 ...
//       g.ResetChain();                    // 新ターン
//   }
//
// 設計 (Pillar ジャンルキット v1):
//   ・**1D 連続配列**: row-major (`y * width + x`)、`acs::TArray<FGridCell>` 1 本。
//     範囲外 Get は内部 static の dummy empty cell を const-ref 返却 (安全)。
//   ・**マッチ検出**: O(w*h) の水平 / 垂直 2 パス走査。3 個以上の連続を 1 個の
//     `FMatchInfo` として吐く (length>=3, horizontal フラグ)。出力バッファ満杯
//     なら検出継続 (count は上限まで)。重複検出は走査構造上発生しない。
//   ・**ESpecialKind 効果**: 消去時に Bomb→周囲 3x3 / Lightning→同行 + 同列 /
//     Rainbow→同色全消去。波及で他の Special を巻き込んだ場合は recursive に
//     チェーン (visited 配列で再入防止)。
//   ・**重力**: 列ごとに「下から上に空を吸い上げる」1-step ApplyGravity と、
//     上端に空が残った場合の RefillFromTop を分離。ResolveAllMatches では
//     両者を fix-point 反復し、再 detect → 連鎖を回す。
//   ・**chain counter**: ResolveAllMatches の各 cascade ループで +1。
//     呼び出し側が ResetChain() しない限り `ChainLevel()` は累積する
//     (1 swap = 1 ターン想定でユーザが明示 reset)。
//   ・**FillRandom の no-match invariant**: 各 cell をランダムに埋めつつ、
//     左 2 個と上 2 個が同色のときは色を 1 個ずらす (色数 >= 2 を要求)。
//     これにより初期 3-連続が出ない決定論的埋め。
//   ・**非コピー・非ムーブ / 全 noexcept / STL 不使用 / acs::TArray のみ**: 規約準拠。
//
// 範囲外 (将来拡張):
//   ・5-連続による Lightning 生成 / T 字 L 字による Bomb 生成などの
//     `combo creation rule` (現状は Special を SetCell 経由で外部から仕込む)。
//   ・hint / no-move 検出 (詰み判定)。
//   ・animation step (ResolveAllMatches を 1 step ずつ進める coroutine API)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Random.h"

namespace acs::game {

/**
 * グリッドのセル 1 個の状態。
 *
 * @details
 * color = 0 は「空」と等価 (empty フラグと冗長だが API 都合で両方持つ)。special は
 * ESpecialKind の u8 表現 (sizeof 削減のため enum 値を生で保持)。
 */
struct FGridCell {
    /** 色 ID (1..color_count、0 = 空)。 */
    u8   color   = 0;

    /** 特殊種別 (ESpecialKind の enum 値)。 */
    u8   special = 0;

    /** 空フラグ (color == 0 と等価だが冪等性のため明示)。 */
    bool empty   = true;
};

/**
 * 連続マッチ 1 個の記述 (length >= 3 の 3-of-a-kind 以上のみ報告)。
 *
 * @details
 * horizontal=true なら (start_x, start_y) から右に length 個、false なら同点から
 * 下に length 個が同色。
 */
struct FMatchInfo {
    /** マッチ開始セルの X 座標。 */
    u32  start_x    = 0;

    /** マッチ開始セルの Y 座標。 */
    u32  start_y    = 0;

    /** 連続している長さ (3 以上)。 */
    u32  length     = 0;

    /** 水平マッチなら true、垂直マッチなら false。 */
    bool horizontal = true;

    /** マッチした色 ID。 */
    u8   color      = 0;
};

/**
 * セルの特殊種別。
 *
 * @details 消去時の波及効果を決める (Normal は波及なし)。
 */
enum class ESpecialKind : u8 {
    /** 普通の宝石 (波及なし)。 */
    Normal    = 0,

    /** 周囲 3x3 範囲を消去。 */
    Bomb      = 1,

    /** 同行 + 同列を消去。 */
    Lightning = 2,

    /** 盤面の同色を全消去。 */
    Rainbow   = 3,
};

/**
 * match-3 パズルのコアロジックを担う 2D グリッド。
 *
 * @details
 * 隣接 swap → マッチ検出 → 消去 (special 効果) → 重力 + 補充 → cascade 再検出を
 * stable まで反復する。セルは row-major の 1D 配列 1 本で保持し、全 noexcept /
 * 非コピー・非ムーブ。
 */
class CMatchGrid {
public:
    /**
     * 消去 1 個ごとに呼ばれる callback の型 (VFX / SFX / score hook 用)。
     *
     * @param user SetOnClearCallback で与えた opaque ポインタ。
     * @param x 消去したセルの X 座標。
     * @param y 消去したセルの Y 座標。
     * @param color 消去したセルの色 ID。
     * @param special 消去したセルの特殊種別。
     */
    using ClearCallback = void(*)(void* user, u32 x, u32 y, u8 color, ESpecialKind special) noexcept;

    /** 空 (サイズ 0) のグリッドを構築する。 */
    CMatchGrid() noexcept = default;

    /** 破棄する (セル配列は TArray が解放)。 */
    ~CMatchGrid() noexcept = default;

    /** コピー禁止。 */
    CMatchGrid(const CMatchGrid&)            = delete;

    /** コピー代入も禁止。 */
    CMatchGrid& operator=(const CMatchGrid&) = delete;

    /** ムーブ禁止。 */
    CMatchGrid(CMatchGrid&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CMatchGrid& operator=(CMatchGrid&&)      = delete;

    /**
     * グリッドを (width x height) で初期化する。
     *
     * @details
     * 不正値 (0) は安全な既定にフォールバックする (width/height は 1、color_count は
     * 1)。color_count は u8 範囲に丸める。chain / clear カウンタもリセットし、全 cell を
     * 空にする。
     * @param width グリッドの幅。
     * @param height グリッドの高さ。
     * @param color_count 利用する色数 (色 ID は 1..color_count)。
     */
    void Init(u32 width, u32 height, u32 color_count) noexcept;

    /**
     * グリッドの幅を返す。
     *
     * @return 幅 (セル数)。
     */
    u32 Width()  const noexcept { return m_Width; }

    /**
     * グリッドの高さを返す。
     *
     * @return 高さ (セル数)。
     */
    u32 Height() const noexcept { return m_Height; }

    /**
     * 指定座標のセルを返す。
     *
     * @param x セルの X 座標。
     * @param y セルの Y 座標。
     * @return 範囲内なら該当 cell の const 参照、範囲外なら内部 static の空 cell。
     */
    const FGridCell& Get(u32 x, u32 y) const noexcept;

    /**
     * 個別 cell を書き換える。
     *
     * @details color==0 のとき empty=true / special=Normal を強制する。範囲外は no-op。
     * @param x セルの X 座標。
     * @param y セルの Y 座標。
     * @param color 設定する色 ID (0 で空)。
     * @param special 設定する特殊種別 (既定 Normal)。
     */
    void Set(u32 x, u32 y, u8 color, ESpecialKind special = ESpecialKind::Normal) noexcept;

    /**
     * 全 cell を 1..color_count のランダム色で埋める。
     *
     * @details
     * 初期 3-連続が発生しないよう、各 cell の左 2 個 / 上 2 個が同色のときは色を 1 個
     * ずらす (color_count >= 2 を前提、1 のときは invariant を諦める)。
     * @param seed 0 なら FRandom::Global()、それ以外は新規 FRandom(seed) で決定論埋め (既定 0)。
     */
    void FillRandom(u32 seed = 0) noexcept;

    /**
     * 隣接 cell の swap を試みる。
     *
     * @details マッチが発生した場合のみ確定して ResolveAllMatches まで完了する。
     * @param x1 1 つ目のセルの X 座標。
     * @param y1 1 つ目のセルの Y 座標。
     * @param x2 2 つ目のセルの X 座標。
     * @param y2 2 つ目のセルの Y 座標。
     * @return マッチが発生し解決まで完了したら true、非隣接 / 範囲外 / マッチ非発生 (swap を戻す) なら false。
     */
    bool TrySwap(u32 x1, u32 y1, u32 x2, u32 y2) noexcept;

    /**
     * 3+ 連続マッチを検出して個数を返す。
     *
     * @details out_matches=nullptr / max_matches=0 のときは count のみ返す。max_matches を超える検出は書き込まれない。
     * @param out_matches 検出結果の書き込み先 (nullptr 可)。
     * @param max_matches out_matches に書き込める上限個数。
     * @return 検出したマッチの総数。
     */
    u32 DetectMatches(FMatchInfo* out_matches, u32 max_matches) const noexcept;

    /**
     * detect → clear (+ Special 効果) → gravity → refill のサイクルを stable まで反復する。
     *
     * @details 各 cascade ループで chain level を +1 する。
     * @return この呼び出しで除去したセルの総数。
     */
    u32 ResolveAllMatches() noexcept;

    /**
     * 列ごとに非空 cell を下に詰めて空きを上に押し上げる 1 step を実行する。
     *
     * @return 移動したセル数 (0 で stable)。
     */
    u32 ApplyGravity() noexcept;

    /**
     * 上端 (y=0) の空 cell をランダム色で埋める 1 step を実行する。
     *
     * @return 埋めたセル数。
     */
    u32 RefillFromTop() noexcept;

    /**
     * 消去時に呼ぶ callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback に渡す opaque ポインタ。
     */
    void SetOnClearCallback(ClearCallback cb, void* user) noexcept;

    /**
     * 現在の chain 中の累計除去数を返す。
     *
     * @return ResetChain 以降に除去したセルの総数。
     */
    u32 TotalClearedThisChain() const noexcept { return m_TotalCleared; }

    /**
     * 現在の chain の cascade 段数を返す。
     *
     * @return ResetChain 以降の ResolveAllMatches 内部ループ回数の累計。
     */
    u32 ChainLevel() const noexcept { return m_ChainLevel; }

    /**
     * 新ターン用に chain / total カウンタを 0 に戻す。
     */
    void ResetChain() noexcept;

    /**
     * 全 cell を空にする (サイズ / color_count / chain は保持)。
     */
    void ClearAll() noexcept;

private:
    /**
     * (x, y) を 1D 配列の index に変換する。
     *
     * @param x セルの X 座標。
     * @param y セルの Y 座標。
     * @return row-major の index (y*width + x)。
     */
    usize Idx(u32 x, u32 y) const noexcept {
        return static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x);
    }

    /**
     * セルを 1 個消去し callback / counter を更新する。
     *
     * @details
     * special 効果からの再入を防ぐため visited で二度同じ座標を消さない。既に空 /
     * visited のときは no-op。special セルなら波及効果も適用する。
     * @param x 消去するセルの X 座標。
     * @param y 消去するセルの Y 座標。
     * @param visited 消去済みフラグ配列 (再入防止)。
     */
    void ClearOne(u32 x, u32 y, TArray<u8>& visited) noexcept;

    /**
     * ESpecialKind 効果を適用して波及範囲を ClearOne する。
     *
     * @param x 起点セルの X 座標。
     * @param y 起点セルの Y 座標。
     * @param color 起点セルの色 ID (Rainbow の対象色)。
     * @param sp 適用する特殊種別。
     * @param visited 消去済みフラグ配列 (再入防止)。
     */
    void ApplySpecialEffect(u32 x, u32 y, u8 color, ESpecialKind sp, TArray<u8>& visited) noexcept;

    /**
     * (x, y) に置く色を、左 2 個 / 上 2 個と被らないように選ぶ (FillRandom 内部用)。
     *
     * @param x 対象セルの X 座標。
     * @param y 対象セルの Y 座標。
     * @param rng 使用する乱数生成器。
     * @return マッチを作らない色 ID (color_count == 1 のときは避けられず単色)。
     */
    u8 PickColorAvoidingMatch(u32 x, u32 y, FRandom& rng) const noexcept;

    /** セル配列 (row-major、y*width + x)。 */
    TArray<FGridCell> m_Cells {};

    /** グリッドの幅。 */
    u32             m_Width        = 0;

    /** グリッドの高さ。 */
    u32             m_Height       = 0;

    /** 利用する色数 (色 ID は 1..color_count)。 */
    u32             m_ColorCount  = 1;

    /** 現在の chain の cascade 段数。 */
    u32             m_ChainLevel  = 0;

    /** 現在の chain の累計除去数。 */
    u32             m_TotalCleared = 0;

    /** 消去 callback (nullptr なら未登録)。 */
    ClearCallback   m_OnClear     = nullptr;

    /** 消去 callback に渡す opaque ポインタ。 */
    void*           m_OnClearUser = nullptr;
};

/** 旧名を使う既存ソースとの互換alias。 */
using FMatchGrid = CMatchGrid;

} // namespace acs::game
