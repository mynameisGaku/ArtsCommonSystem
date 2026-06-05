// SPDX-License-Identifier: Apache-2.0
// ACS Memory — アロケータ抽象インターフェイス
//
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
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "foundation/Assert.h"

namespace acs {

/**
 * 既定アライメント (バイト)。
 *
 * @details SIMD (16B) と一般用途 (ポインタ幅) の妥協点として 16 を採用する
 * (ポインタが 8B 超のプラットフォームでは alignof(void*) を使う)。
 */
inline constexpr usize kDefaultAlignment = alignof(void*) > 8 ? alignof(void*) : 16;

/**
 * メモリ確保の抽象インターフェイス (純粋仮想)。
 *
 * @details
 * すべてのコンテナ・スマートポインタ・サブシステムはこのインターフェイス越しに
 * 確保/解放を行うため、具象アロケータ (プール/アリーナ等) を呼び出し側を変えずに
 * 差し替えられる。具象実装はスレッドセーフで、確保失敗時は nullptr を返し例外は投げない。
 */
class FAllocator {
public:
    /** 派生アロケータを正しく破棄するための仮想デストラクタ。 */
    virtual ~FAllocator() noexcept = default;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント (2 のべき乗)。
     * @param loc 診断用の呼び出し位置 (リーク追跡やプロファイラに渡される)。
     * @return 確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    virtual void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept = 0;

    /**
     * ptr を解放する。
     *
     * @param ptr 解放する領域 (nullptr は no-op)。
     */
    virtual void Free(void* ptr) noexcept = 0;

    /**
     * 既存ポインタを new_size に拡張・縮小する。
     *
     * @details 既定実装は Alloc + memcpy + Free (Memory.cpp で定義)。
     * @param ptr 既存の確保領域 (nullptr なら新規確保扱い)。
     * @param old_size ptr の現在のバイト数。
     * @param new_size 確保し直す新しいバイト数。
     * @param alignment 要求アライメント (2 のべき乗)。
     * @param loc 診断用の呼び出し位置。
     * @return 再確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    virtual void* Realloc(void* ptr, usize old_size, usize new_size,
                          usize alignment, FSourceLoc loc) noexcept;

    /**
     * 現在の使用バイト数を返す。
     *
     * @return 現在の確保済みバイト数 (既定は 0。実装側で上書き)。
     */
    virtual u64 BytesAllocated() const noexcept { return 0; }

    /**
     * ピーク使用バイト数を返す。
     *
     * @return これまでの最大確保済みバイト数 (既定は 0。実装側で上書き)。
     */
    virtual u64 PeakBytes()      const noexcept { return 0; }

    /**
     * アロケータの識別名を返す。
     *
     * @return アロケータ名 (既定は "FAllocator"。実装側で上書き)。
     */
    virtual const char* Name()   const noexcept { return "FAllocator"; }

    /**
     * 既定アライメントで size バイトを確保する利便性オーバーロード。
     *
     * @param size 確保するバイト数。
     * @param loc 診断用の呼び出し位置 (既定で呼び出し元を自動キャプチャ)。
     * @return 確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    ACS_FORCEINLINE void* Alloc(usize size, FSourceLoc loc = FSourceLoc::Current()) noexcept {
        return Alloc(size, kDefaultAlignment, loc);
    }
};

/**
 * v が 2 のべき乗かを判定する。
 *
 * @param v 判定する値。
 * @return 2 のべき乗 (0 は除く) なら true。
 */
ACS_FORCEINLINE bool IsPow2(usize v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

/**
 * n を a の倍数に切り上げる。
 *
 * @details a は 2 のべき乗である必要があり、(n + a-1) がラップしないことを assert する。
 * @param n 切り上げる値。
 * @param a アライメント (2 のべき乗)。
 * @return a の倍数に切り上げた値。
 */
ACS_FORCEINLINE usize AlignUp(usize n, usize a) noexcept {
    ACS_ASSERT(a != 0 && (a & (a - 1)) == 0);  // a はマスクが成立する 2 のべき乗である必要
    ACS_ASSERT(n <= (~usize(0)) - (a - 1));     // (n + a-1) がラップしてはならない
    return (n + (a - 1)) & ~(a - 1);
}

/**
 * ポインタ p を a の倍数アドレスに切り上げる。
 *
 * @param p 切り上げるポインタ。
 * @param a アライメント (2 のべき乗)。
 * @return a 整列に切り上げたポインタ。
 */
ACS_FORCEINLINE void* AlignUp(void* p, usize a) noexcept {
    return reinterpret_cast<void*>(AlignUp(reinterpret_cast<uptr>(p), a));
}

} // namespace acs
