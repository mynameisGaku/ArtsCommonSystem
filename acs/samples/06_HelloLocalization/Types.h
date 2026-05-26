// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — 埋め込み言語データ。
//
// 実プロジェクトではファイルから LoadFromBytes するのが普通だが、
// このサンプルでは「実行ファイル組み込み」を見せたいので、ja/en/fr の
// 3 ロケールを const char* に直書きしている。
#pragma once

namespace helloloc {

// 日本語 — full
extern const char* const kLangJa;
// English — full (fallback でも使う)
extern const char* const kLangEn;
// Français — partial (一部キー未訳で fallback テスト)
extern const char* const kLangFr;

} // namespace helloloc
