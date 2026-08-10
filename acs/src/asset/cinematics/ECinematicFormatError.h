// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::asset {

/** .cine 検証で分類する入力エラーです。 */
enum class ECinematicFormatError : u8 {
    /** エラーなしの状態。 */
    None,
    /** magicが一致しない入力。 */
    InvalidMagic,
    /** 対応しないversion。 */
    UnsupportedVersion,
    /** headerまたは予約値が不正な入力。 */
    InvalidHeader,
    /** 定義外のイベント種別。 */
    UnknownKind,
    /** 時刻順序が不正な入力。 */
    InvalidOrder,
    /** 数値が有限範囲外の入力。 */
    InvalidNumber,
    /** 文字列がUTF-8またはNUL規約に違反する入力。 */
    InvalidText,
    /** 種別に合わないイベントpayload。 */
    InvalidEvent,
    /** 許容サイズを超えた入力。 */
    SizeLimit,
    /** 入力が途中で終わった状態。 */
    Truncated,
    /** 規定領域の後に残るデータ。 */
    TrailingData,
    /** メモリ確保に失敗した状態。 */
    OutOfMemory
};

} // namespace acs::asset
