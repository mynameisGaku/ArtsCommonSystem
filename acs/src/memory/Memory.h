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
 * nullptr を渡すと内部の FSystemAllocator に戻る。起動初期 (コンテナ等が確保を
 * 始める前) に呼ぶこと。スレッドセーフではない。
 * @param a 新しいデフォルトアロケータ (nullptr で FSystemAllocator に戻す)。
 */
void SetDefaultAllocator(FAllocator* a) noexcept;

/**
 * 領域非重複コピー (::memcpy への薄いラッパ)。
 *
 * @details dst と src が重複する場合の動作は未定義。重複可能性があれば MemMove を使う。
 * @param dst コピー先 (src と重複してはならない)。
 * @param src コピー元。
 * @param n コピーするバイト数。
 */
void MemCopy(void* dst, const void* src, usize n) noexcept;

/**
 * 領域重複可能コピー (::memmove への薄いラッパ)。
 *
 * @param dst コピー先 (src と重複してもよい)。
 * @param src コピー元。
 * @param n コピーするバイト数。
 */
void MemMove(void* dst, const void* src, usize n) noexcept;

/**
 * バイト単位フィル (::memset への薄いラッパ)。
 *
 * @param dst 書き込み先。
 * @param value 各バイトに書き込む値 (unsigned char に切り詰められる)。
 * @param n 書き込むバイト数。
 */
void MemSet (void* dst, int   value,    usize n) noexcept;

/**
 * バイト比較 (::memcmp への薄いラッパ)。
 *
 * @param a 比較対象 A。
 * @param b 比較対象 B。
 * @param n 比較するバイト数。
 * @return a<b なら負、等しければ 0、a>b なら正。
 */
int  MemCmp (const void* a, const void* b, usize n) noexcept;

} // namespace acs
