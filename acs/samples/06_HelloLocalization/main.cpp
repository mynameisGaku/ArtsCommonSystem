// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — エントリポイント。
//
// 構成:
//   Types.{h,cpp}                  - 埋め込み言語データ (kLangJa/En/Fr)
//   HelloLocalizationApp.{h,cpp}   - FApplication 派生クラス
//
// 操作:
//   F1  : 日本語
//   F2  : 英語
//   F3  : フランス語
//   Esc : 終了
#include "HelloLocalizationApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloloc::HelloLocalizationApp)
