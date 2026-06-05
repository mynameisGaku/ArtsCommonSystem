// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySnapshot（メモリ使用率の可視化出力）
// -----------------------------------------------------------------------------
// FMemorySystem の現状を SVG / BMP として出力する。SVG はテキストベースで
// 軽量・ブラウザで開ける、ラベル付きで人間に読みやすい。BMP は外部ツールで
// パイプライン化しやすい。両方ゼロ依存（自前ライタ）。
//
// 視覚化レイアウト:
//   各セグメントを 1 行のバーとして描画。バー全長が予約量、塗りつぶしが
//   使用量、背景が予算上限。色はセグメントごとに固定。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

/** FMemorySystem の使用状況を SVG/BMP/stdout として可視化出力するユーティリティ (全 static、ゼロ依存)。 */
class FMemorySnapshot {
public:
    /**
     * メモリ使用状況を SVG ファイルに出力する (人間可読、ラベル付き)。
     *
     * @details セグメントごとに 1 行のバー (予約=全長、使用=塗り、ピーク=赤線) を描く。セグメントが無ければエラー。
     * @param path 出力先のファイルパス (上書き作成)。
     * @param width 画像の幅 (既定 800)。
     * @param row_height 1 行の高さ (既定 40)。
     * @return 成功なら空の TResult、セグメント無し/ファイル作成失敗ならエラー。
     */
    static TResult<void> WriteSvg(const wchar_t* path,
                                 u32 width = 800,
                                 u32 row_height = 40) noexcept;

    /**
     * メモリ使用状況を 24bpp 無圧縮 BMP ファイルに出力する (外部依存ゼロ)。
     *
     * @details セグメントごとに 1 行のバー (予約=灰背景、使用=カラー) を描く。セグメントが無ければエラー。
     * @param path 出力先のファイルパス (上書き作成)。
     * @param width 画像の幅 (既定 800)。
     * @param row_height 1 行の高さ (既定 30)。
     * @return 成功なら空の TResult、セグメント無し/確保/ファイル作成失敗ならエラー。
     */
    static TResult<void> WriteBmp(const wchar_t* path,
                                 u32 width = 800,
                                 u32 row_height = 30) noexcept;

    /**
     * メモリ使用状況をコンソールへテキスト表として出力する (CI ログ/ターミナル用)。
     */
    static void DumpToStdOut() noexcept;
};

} // namespace acs
