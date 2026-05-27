// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (puzzle / match-3) — FMatchGrid
//
// `Bejeweled` / `Candy Crush` 系 match-3 パズルのコアロジックを担う 2D グリッド。
// セルは「色 (1..color_count)」「special 種別 (Normal / Bomb / Lightning /
// Rainbow)」「空フラグ」の 3 値を持ち、以下のサイクルを内包する:
//   1) 隣接 swap (TrySwap) → match が発生したら確定、無ければ戻す。
//   2) 3 個以上の縦横連続を検出 (DetectMatches) → MatchInfo の列を吐く。
//   3) ResolveAllMatches でマッチを順次消化:
//        - ESpecialKind 効果 (Bomb=3x3 / Lightning=行+列 / Rainbow=同色全消し) を適用
//        - 空になった cell に対し ApplyGravity / RefillFromTop で重力 + 補充
//        - 再度マッチが発生するか走査 (cascade) → stable まで反復
//      返却値は除去総数。chain level も内部カウンタで保持し連鎖判定に使える。
//
// 使い方:
//   FMatchGrid g;
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
//   ・**1D 連続配列**: row-major (`y * width + x`)、`acs::TArray<GridCell>` 1 本。
//     範囲外 Get は内部 static の dummy empty cell を const-ref 返却 (安全)。
//   ・**マッチ検出**: O(w*h) の水平 / 垂直 2 パス走査。3 個以上の連続を 1 個の
//     `MatchInfo` として吐く (length>=3, horizontal フラグ)。出力バッファ満杯
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

// セル 1 個の状態。
// `color = 0` は「空」と等価 (empty フラグと冗長だが API 都合で両方持つ)。
// `special` は ESpecialKind の u8 表現 (sizeof 削減のため enum 値を生で保持)。
struct GridCell {
    u8   color   = 0;       // 1..color_count = 色、0 = 空
    u8   special = 0;       // ESpecialKind enum 値
    bool empty   = true;    // color == 0 と等価だが冪等性のため明示
};

// 連続マッチ 1 個の記述。length >= 3 (3-of-a-kind 以上のみ報告)。
// horizontal=true なら (start_x, start_y) から右に length 個、
// false なら同点から下に length 個が同色。
struct MatchInfo {
    u32  start_x    = 0;
    u32  start_y    = 0;
    u32  length     = 0;
    bool horizontal = true;
    u8   color      = 0;
};

// セルの「特殊」種別。Normal=普通の宝石、Bomb=3x3 範囲消去、
// Lightning=同行+同列消去、Rainbow=盤面の同色全消去。
enum class ESpecialKind : u8 {
    Normal    = 0,
    Bomb      = 1,
    Lightning = 2,
    Rainbow   = 3,
};

class FMatchGrid {
public:
    // 消去 1 個ごとの callback (VFX / SFX / score hook 想定)。
    // user は SetOnClearCallback で与えた opaque ポインタ。
    using ClearCallback = void(*)(void* user, u32 x, u32 y, u8 color, ESpecialKind special) noexcept;

    FMatchGrid() noexcept = default;
    ~FMatchGrid() noexcept = default;

    // 非コピー・非ムーブ
    FMatchGrid(const FMatchGrid&)            = delete;
    FMatchGrid& operator=(const FMatchGrid&) = delete;
    FMatchGrid(FMatchGrid&&)                 = delete;
    FMatchGrid& operator=(FMatchGrid&&)      = delete;

    // グリッドを (width x height) で初期化。`color_count` は 1..255 の範囲で
    // 利用する色数 (色 ID は 1..color_count)。不正値 (0) は安全な既定にフォールバック
    // (width/height は 1、color_count は 1)。chain / clear カウンタもリセット。
    void Init(u32 width, u32 height, u32 color_count) noexcept;

    u32 Width()  const noexcept { return m_Width; }
    u32 Height() const noexcept { return m_Height; }

    // 範囲内なら該当 cell の const-ref。範囲外なら内部 static の dummy empty cell。
    const GridCell& Get(u32 x, u32 y) const noexcept;

    // 個別 cell 書き換え。color==0 のとき empty=true / special=Normal を強制。
    // 範囲外は no-op。
    void Set(u32 x, u32 y, u8 color, ESpecialKind special = ESpecialKind::Normal) noexcept;

    // 全 cell を 1..color_count のランダム色で埋める。
    // 初期 3-連続が発生しないよう、各 cell の左 2 個 / 上 2 個が同色のときは
    // 色を 1 個ずらす。color_count >= 2 を前提とする (1 のときは invariant を諦め）。
    // seed=0 のときは FRandom::Global() を使用、それ以外は新規 FRandom(seed) で決定論。
    void FillRandom(u32 seed = 0) noexcept;

    // 隣接 cell の swap を試みる。
    // 戻り値 true: マッチが発生し ResolveAllMatches まで完了。
    // 戻り値 false: 非隣接 / 範囲外 / マッチ非発生 (swap は戻される)。
    bool TrySwap(u32 x1, u32 y1, u32 x2, u32 y2) noexcept;

    // 3+ 連続マッチを `out_matches` に書き出して個数を返す。
    // `max_matches` 上限で打ち切り (それ以降の検出は失われる)。
    // out_matches=nullptr / max_matches=0 のときは count のみ返す。
    u32 DetectMatches(MatchInfo* out_matches, u32 max_matches) const noexcept;

    // detect → clear (+ Special 効果) → gravity → refill のサイクルを stable に
    // なるまで反復する。返却は除去総数。各 cascade ループで chain level += 1。
    u32 ResolveAllMatches() noexcept;

    // 列ごとに空 cell を上向きに「吸い上げ」、空きを上に押し上げる 1 step。
    // 返却は移動した cell 数 (= 0 で stable)。
    u32 ApplyGravity() noexcept;

    // 上端 (y=0) の空 cell をランダム色で埋める 1 step。
    // 返却は埋めた cell 数。
    u32 RefillFromTop() noexcept;

    // VFX / SFX / score hook 用 callback を登録。cb=nullptr で解除。
    void SetOnClearCallback(ClearCallback cb, void* user) noexcept;

    // この chain (= ResetChain 以降) 中の累計除去数。
    u32 TotalClearedThisChain() const noexcept { return m_TotalCleared; }

    // この chain 中の cascade 段数 (= ResolveAllMatches の内部ループ回数の累計)。
    u32 ChainLevel() const noexcept { return m_ChainLevel; }

    // 新ターン用に chain / total カウンタを 0 に戻す。
    void ResetChain() noexcept;

    // 全 cell を empty に。サイズ / color_count / chain は保持。
    void ClearAll() noexcept;

private:
    // 1D index (`y*width + x`)
    usize Idx(u32 x, u32 y) const noexcept {
        return static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x);
    }

    // セルを 1 個消去し callback / counter を更新。波及 (special 効果) からの
    // 再入を視覚化するために `visited` を渡し、二度同じ座標を消さない。
    // 既に空 / visited のときは no-op。
    void ClearOne(u32 x, u32 y, TArray<u8>& visited) noexcept;

    // ESpecialKind 効果を適用して波及範囲も ClearOne する。
    void ApplySpecialEffect(u32 x, u32 y, u8 color, ESpecialKind sp, TArray<u8>& visited) noexcept;

    // FillRandom 内部用: (x, y) に色を置くとき左 2 個 / 上 2 個と被らない色を選ぶ。
    u8 PickColorAvoidingMatch(u32 x, u32 y, FRandom& rng) const noexcept;

    TArray<GridCell> m_Cells {};        // row-major (`y * width + x`)
    u32             m_Width        = 0;
    u32             m_Height       = 0;
    u32             m_ColorCount  = 1;
    u32             m_ChainLevel  = 0;
    u32             m_TotalCleared = 0;
    ClearCallback   m_OnClear     = nullptr;
    void*           m_OnClearUser = nullptr;
};

} // namespace acs::game
