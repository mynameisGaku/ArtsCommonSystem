// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 既知の ASCII 拡張子分類。非 ASCII や曖昧な末尾は Unknown のまま扱う。 */
enum class EFileExtensionKind : u8 {
    /** 既知形式へ安全に分類できない拡張子。 */
    Unknown,

    /** INI 設定ファイル。 */
    Ini,

    /** cfg 設定ファイル。 */
    Config,

    /** JSON 文書。 */
    Json,

    /** プレーンテキスト。 */
    Text,

    /** 任意バイナリ。 */
    Binary,

    /** ACS の資産パック。 */
    AssetPack,
};

} // namespace acs
