// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <type_traits>

namespace acs {

/**
 * 文字列設定を一括反映するときのキーと値を表す。
 *
 * @details key は有効な UTF-8 の終端文字列を指定し、Save/Load 後も identity を維持するため
 * 先頭と末尾の ASCII space (U+0020) を許可しない。内部の ASCII space は許可する。
 * value の nullptr は空文字列として扱い、非 nullptr は既存の単体 setter と同じく終端までの
 * byte 列を追加検査や変換なしで保持する。二つの pointer を宣言順に保持する aggregate であり、
 * standard-layout と trivially-copyable の ABI をコンパイル時に検査する。
 */
struct FStorageStringBatchEntry {
    /** 設定対象の終端文字列キー。先頭と末尾に ASCII space (U+0020) は指定できない。 */
    const char* key;

    /** 設定する終端 byte 列。nullptr は空文字列として扱い、非 nullptr には追加制約を設けない。 */
    const char* value;
};

static_assert(sizeof(FStorageStringBatchEntry) == sizeof(const char*) * 2u);
static_assert(alignof(FStorageStringBatchEntry) == alignof(const char*));
static_assert(offsetof(FStorageStringBatchEntry, key) == 0u);
static_assert(offsetof(FStorageStringBatchEntry, value) == sizeof(const char*));
static_assert(std::is_standard_layout_v<FStorageStringBatchEntry>);
static_assert(std::is_trivially_copyable_v<FStorageStringBatchEntry>);
static_assert(std::is_aggregate_v<FStorageStringBatchEntry>);

} // namespace acs
