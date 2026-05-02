// =============================================================================
// ACS Memory — グローバルアロケータ取得 + 低レベルメモリ操作
// -----------------------------------------------------------------------------
// 提供物:
//   - DefaultAllocator()       — エンジン全体のデフォルトアロケータ取得
//   - SetDefaultAllocator()    — 差し替え（起動時に呼ぶ前提）
//   - MemCopy / MemMove / MemSet / MemCmp — STL <cstring> ラッパ
//
// memcpy 等は Hot path で使うため、関数呼び出しコストを CRT 実装に委ねる。
// CRT 実装は SIMD で最適化済み。自前実装より速いケースが多い。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "memory/Allocator.h"

namespace acs {

// 現在のデフォルトアロケータを返す（起動時は SystemAllocator）。
// 全コンテナのデフォルトコンストラクタで使われる。
Allocator& DefaultAllocator() noexcept;

// デフォルトを差し替える（nullptr で SystemAllocator に戻す）。
// アロケーション中に呼び出すのは未定義動作なので、起動初期に行うこと。
void SetDefaultAllocator(Allocator* a) noexcept;

// ---- 低レベルメモリ操作（<cstring> を直接 include しないラッパ） ---------
void MemCopy(void* dst, const void* src, usize n) noexcept;  // 領域非重複コピー
void MemMove(void* dst, const void* src, usize n) noexcept;  // 領域重複可能コピー
void MemSet (void* dst, int   value,    usize n) noexcept;   // バイト単位フィル
int  MemCmp (const void* a, const void* b, usize n) noexcept; // バイト比較

} // namespace acs
