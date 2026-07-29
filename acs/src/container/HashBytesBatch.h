// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Hash.h"

namespace acs {

/** バッチハッシュ一件分の入力範囲とシード。 */
struct FHashBytesInput {
    /** ハッシュ対象の先頭。 */
    const void* data = nullptr;
    /** ハッシュ対象のバイト数。 */
    usize length = 0u;
    /** 一件固有の初期シード。 */
    u64 seed = 0xCBF29CE484222325ull;
};

/**
 * 独立した複数範囲のハッシュをまとめて計算する。
 *
 * @param inputs 入力範囲とシードの配列。
 * @param count 入出力の要素数。
 * @param output HashBytes と同じ値を書き込む配列。
 */
void HashBytesBatch(const FHashBytesInput* inputs, usize count, u64* output) noexcept;

/**
 * 四個の整数キーを依存鎖ごとに交互実行して混合する。
 *
 * @param input 四個の整数キー。
 * @param output HashMix64 と同じ値を書き込む四要素配列。
 */
void HashMix64Batch4(const u64 (&input)[4], u64 (&output)[4]) noexcept;

} // namespace acs
