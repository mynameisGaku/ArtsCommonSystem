// SPDX-License-Identifier: Apache-2.0
// グローバルアロケータ取得 + 低レベルメモリ操作
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "memory/Allocator.h"

namespace acs {

/**
 * 現在のデフォルトアロケータを返す。
 *
 * @details 起動時は内部のプロセス全体 FSystemAllocator を指す。
 * @return デフォルトアロケータへの参照。
 */
FAllocator& DefaultAllocator() noexcept;

/**
 * デフォルトアロケータを差し替える。
 *
 * @details
 * nullptr を渡すと内部の FSystemAllocator に戻る。DefaultAllocator との並行呼出でも
 * ポインタの公開はスレッドセーフ。差し替え前のアロケータを参照中の処理は継続し得るため、
 * 呼出側は全利用者が停止するまで差し替え先と差し替え前のアロケータを生存させること。
 * @param Allocator 新しいデフォルトアロケータ (nullptr で FSystemAllocator に戻す)。
 */
void SetDefaultAllocator(FAllocator* Allocator) noexcept;

/**
 * 領域非重複コピー (::memcpy への薄いラッパ)。
 *
 * @details Destination と Source が重複する場合の動作は未定義。重複可能性があれば MemMove を使う。
 * @param Destination コピー先 (Source と重複してはならない)。
 * @param Source コピー元。
 * @param Size コピーするバイト数。
 */
void MemCopy(void* Destination, const void* Source, usize Size) noexcept;

/**
 * 領域重複可能コピー (::memmove への薄いラッパ)。
 *
 * @param Destination コピー先 (Source と重複してもよい)。
 * @param Source コピー元。
 * @param Size コピーするバイト数。
 */
void MemMove(void* Destination, const void* Source, usize Size) noexcept;

/**
 * バイト単位フィル (::memset への薄いラッパ)。
 *
 * @param Destination 書き込み先。
 * @param Value 各バイトに書き込む値 (unsigned char に切り詰められる)。
 * @param Size 書き込むバイト数。
 */
void MemSet(void* Destination, int Value, usize Size) noexcept;

/**
 * バイト比較 (::memcmp への薄いラッパ)。
 *
 * @param Left 比較対象の左辺。
 * @param Right 比較対象の右辺。
 * @param Size 比較するバイト数。
 * @return left<right なら負、等しければ 0、left>right なら正。
 */
int MemCmp(const void* Left, const void* Right, usize Size) noexcept;

} // namespace acs
