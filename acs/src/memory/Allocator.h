// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — アロケータ抽象インターフェイス
// -----------------------------------------------------------------------------
// すべてのコンテナ・スマートポインタ・サブシステムは FAllocator* を介して
// メモリ確保を行う。これによりプール / アリーナ / サンドボックスアロケータ
// を呼び出し側を再テンプレート化することなく差し替えられる。
//
// 実装規約:
//   - 全ての具象アロケータは「スレッドセーフ」であること（並列確保 OK）
//   - alignment は 2 のべき乗
//   - Alloc 失敗時は nullptr 返却（例外は投げない）
//
// 性能注意:
//   virtual 呼び出しのコスト（vtable 1 段間接 + 仮想関数 prediction miss）が
//   ホットパス（毎フレーム数千回）で問題になる場合は、テンプレート化された
//   薄いアダプタ越しに呼ぶか、ヒューリスティック inline 候補（FPoolAllocator
//   の固定サイズ確保等）を別 API で公開すること。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

// 既定アライメント。SIMD (16B) と一般用途 (8B) の妥協点として 16 を採用。
inline constexpr usize kDefaultAlignment = alignof(void*) > 8 ? alignof(void*) : 16;

// =============================================================================
// FAllocator — 純粋仮想インターフェイス
// =============================================================================
class FAllocator {
public:
    virtual ~FAllocator() noexcept = default;

    // ---- 確保 ----
    // size バイトを alignment 整列で確保する。失敗時は nullptr。
    // loc は診断用（リーク追跡やプロファイラに渡される）。
    virtual void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept = 0;

    // ---- 解放 ----
    // ptr を解放する。nullptr は no-op。
    virtual void Free(void* ptr) noexcept = 0;

    // ---- 再確保 ----
    // 既存ポインタを new_size に拡張・縮小する。
    // デフォルト実装は Alloc + memcpy + Free（Memory.cpp で定義）。
    virtual void* Realloc(void* ptr, usize old_size, usize new_size,
                          usize alignment, FSourceLoc loc) noexcept;

    // ---- 統計 (デフォルトは 0 を返す。実装側で上書き) ----
    virtual u64 BytesAllocated() const noexcept { return 0; }   // 現在の使用量
    virtual u64 PeakBytes()      const noexcept { return 0; }   // ピーク使用量
    virtual const char* Name()   const noexcept { return "FAllocator"; }  // 識別名

    // ---- 利便性オーバーロード ----
    // alignment 既定値版。FSourceLoc は呼び出し位置を自動キャプチャ。
    ACS_FORCEINLINE void* Alloc(usize size, FSourceLoc loc = FSourceLoc::Current()) noexcept {
        return Alloc(size, kDefaultAlignment, loc);
    }
};

// ---- ヘルパ -------------------------------------------------------------

// 2 のべき乗判定
ACS_FORCEINLINE bool IsPow2(usize v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// アラインアップ: n を a の倍数に切り上げる（a は 2 のべき乗である必要がある）
ACS_FORCEINLINE usize AlignUp(usize n, usize a) noexcept {
    return (n + (a - 1)) & ~(a - 1);
}

ACS_FORCEINLINE void* AlignUp(void* p, usize a) noexcept {
    return reinterpret_cast<void*>(AlignUp(reinterpret_cast<uptr>(p), a));
}

} // namespace acs
