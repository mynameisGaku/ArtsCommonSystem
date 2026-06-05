// SPDX-License-Identifier: Apache-2.0
// ACS Memory — ESegment 列挙
//
// メモリを目的別の論理ヒープに分割する。各セグメントには独立した予算と
// 寿命特性があり、フラグメンテーションを「目的・寿命」で隔離する。
//
//   Default     — 通常のゲーム実行時の汎用確保
//   Permanent   — 起動時に一度確保、ゲーム終了まで保持
//   Temp        — 1 フレームで完結する一時データ（毎フレーム Reset）
//   Resource    — モデル・テクスチャ・音声等のアセット
//   Develop     — エディタ／プロファイラ／デバッグ専用
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * メモリを目的・寿命別に隔離する論理ヒープの種別。
 *
 * @details
 * セグメントごとに独立した予算と寿命特性を持たせ、フラグメンテーションを目的・寿命で
 * 隔離する。各値は配列添字として使えるよう 0 から連番で振る。
 */
enum class ESegment : u8 {
    /** 通常のゲーム実行時の汎用確保。 */
    Default   = 0,

    /** 起動時に一度確保し、ゲーム終了まで保持する領域。 */
    Permanent = 1,

    /** 1 フレームで完結する一時データ (毎フレーム Reset される)。 */
    Temp      = 2,

    /** モデル・テクスチャ・音声等のアセット。 */
    Resource  = 3,

    /** エディタ・プロファイラ・デバッグ専用。 */
    Develop   = 4,

    /** セグメント数 (配列サイズ算出用の番兵)。 */
    _Count
};

/**
 * ESegment を表示用の文字列に変換する。
 *
 * @param s 変換するセグメント種別。
 * @return セグメント名の文字列リテラル (未知の値は "?")。
 */
constexpr const char* ToString(ESegment s) noexcept {
    switch (s) {
        case ESegment::Default:   return "Default";
        case ESegment::Permanent: return "Permanent";
        case ESegment::Temp:      return "Temp";
        case ESegment::Resource:  return "Resource";
        case ESegment::Develop:   return "Develop";
        default: return "?";
    }
}

/**
 * 各 alloc ヘッダに付与する用途タグ (プロファイラで分類するため)。
 *
 * @details セグメント (寿命) とは独立に「どのサブシステムの確保か」を記録する分類軸。
 */
enum class EAllocKind : u8 {
    /** 分類不明・汎用の確保。 */
    Generic   = 0,

    /** エンジンコアの確保。 */
    Engine    = 1,

    /** ゲームロジックの確保。 */
    FGame      = 2,

    /** レンダラの確保。 */
    Render    = 3,

    /** オーディオの確保。 */
    Audio     = 4,

    /** アセット (モデル・テクスチャ等) の確保。 */
    Asset     = 5,

    /** UI の確保。 */
    UI        = 6,

    /** ネットワークの確保。 */
    Network   = 7,

    /** デバッグ用の確保。 */
    Debug     = 8,
};

} // namespace acs
