// グローバルアロケータ取得 + 低レベルメモリ操作
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "memory/Allocator.h"

namespace acs {

// 現在のデフォルトアロケータを返す（起動時は SystemAllocator）
Allocator& DefaultAllocator() noexcept;

// デフォルトを差し替える（nullptr で SystemAllocator に戻す、起動初期に呼ぶこと）
void SetDefaultAllocator(Allocator* a) noexcept;

// 領域非重複コピー
void MemCopy(void* dst, const void* src, usize n) noexcept;
// 領域重複可能コピー
void MemMove(void* dst, const void* src, usize n) noexcept;
// バイト単位フィル
void MemSet (void* dst, int   value,    usize n) noexcept;
// バイト比較
int  MemCmp (const void* a, const void* b, usize n) noexcept;

} // namespace acs
