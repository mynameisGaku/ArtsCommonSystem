// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — MemorySnapshot（メモリ使用率の可視化出力）
// -----------------------------------------------------------------------------
// MemorySystem の現状を SVG / BMP として出力する。SVG はテキストベースで
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

class MemorySnapshot {
public:
    // SVG ファイル出力（人間可読、ラベル付き）
    static TResult<void> WriteSvg(const wchar_t* path,
                                 u32 width = 800,
                                 u32 row_height = 40) noexcept;

    // BMP ファイル出力（24bpp、外部依存ゼロ）
    static TResult<void> WriteBmp(const wchar_t* path,
                                 u32 width = 800,
                                 u32 row_height = 30) noexcept;

    // コンソールへテキスト出力（ロガー / ターミナル用）
    static void DumpToStdOut() noexcept;
};

} // namespace acs
