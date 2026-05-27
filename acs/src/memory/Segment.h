// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — ESegment 列挙
// -----------------------------------------------------------------------------
// メモリを目的別の論理ヒープに分割する。各セグメントには独立した予算と
// 寿命特性があり、フラグメンテーションを「目的・寿命」で隔離する。
//
//   Default     — 通常のゲーム実行時の汎用確保
//   Permanent   — 起動時に一度確保、ゲーム終了まで保持
//   Temp        — 1 フレームで完結する一時データ（毎フレーム Reset）
//   Resource    — モデル・テクスチャ・音声等のアセット
//   Develop     — エディタ／プロファイラ／デバッグ専用
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs {

enum class ESegment : u8 {
    Default   = 0,
    Permanent = 1,
    Temp      = 2,
    Resource  = 3,
    Develop   = 4,
    _Count
};

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

// 各 alloc に付与するヘッダの「種別」タグ（プロファイラで分類するため）
enum class EAllocKind : u8 {
    Generic   = 0,
    Engine    = 1,
    FGame      = 2,
    Render    = 3,
    Audio     = 4,
    Asset     = 5,
    UI        = 6,
    Network   = 7,
    Debug     = 8,
};

} // namespace acs
