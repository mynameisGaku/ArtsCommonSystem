// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — システムアロケータ（Win32 プロセスヒープ）
// -----------------------------------------------------------------------------
// HeapAlloc / HeapFree を使った汎用アロケータ。プロセスヒープは OS が
// 内部でロックを取って直列化するため、スレッドセーフ（HEAP_NO_SERIALIZE
// は意図的に未指定）。
//
// アライメント:
//   HeapAlloc 自体は MEMORY_ALLOCATION_ALIGNMENT (x64 で 16B) しか保証しない。
//   それを超えるアライメントが必要な場合は、内部で「アラインド確保 +
//   ヘッダで元ポインタを覚える」方式（AlignedAlloc）を使う。
//
// 性能注意:
//   HeapAlloc は数百 ns のオーバーヘッドがある。フレーム内で何千回も呼ぶ
//   ようなホットパスでは、専用プールやアリーナを別途用意すべき。
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

/**
 * Win32 プロセスヒープ (HeapAlloc/HeapFree) を使う汎用スレッドセーフアロケータ。
 *
 * @details
 * プロセスヒープは OS が内部でロックを取って直列化するためスレッドセーフ
 * (HEAP_NO_SERIALIZE は意図的に未指定)。HeapAlloc は 16B しか整列を保証しないため、
 * それを超えるアライメント要求は内部で「余裕を持って確保 + ヘッダに元ポインタを退避」する
 * 方式で満たす。確保量・ピークはアトミックに集計する。HeapAlloc は数百 ns のコストがあるため、
 * フレーム内で何千回も呼ぶホットパスでは専用プール/アリーナを使うこと。
 */
class FSystemAllocator final : public FAllocator {
public:
    /** 既定構築する (状態は統計カウンタのみ)。 */
    FSystemAllocator() noexcept = default;

    /** 破棄する (プロセスヒープ自体は OS 所有なので解放しない)。 */
    ~FSystemAllocator() noexcept override = default;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @details 16B 超の alignment はヘッダ退避方式で満たす。size==0 や確保失敗時は nullptr。
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント (2 のべき乗)。
     * @param loc 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc  (usize size, usize alignment, FSourceLoc loc) noexcept override;

    /**
     * ptr を解放する。
     *
     * @details ヘッダから元ポインタを取り出して HeapFree に渡す。nullptr は no-op。
     * @param ptr Alloc で得た領域 (nullptr 可)。
     */
    void  Free   (void* ptr)                                  noexcept override;

    /**
     * ptr を new_size に再確保する。
     *
     * @details
     * HeapReAlloc はヘッダ退避方式と相性が悪いため、基底の Alloc+copy+Free 実装に委譲する。
     * @param ptr 既存の確保 (nullptr なら新規確保)。
     * @param old_size 旧サイズ。
     * @param new_size 新サイズ (0 なら解放)。
     * @param alignment 新領域のアライメント。
     * @param loc 診断用の呼び出し位置。
     * @return 新しい確保 (失敗時 nullptr)。
     */
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc)             noexcept override;

    /**
     * 現在の総割当バイト数を返す。
     *
     * @return 生存中の確保の合計バイト数 (ヘッダ・アライメント余裕込みの実ヒープ確保量)。
     */
    u64 BytesAllocated() const noexcept override { return m_Bytes.Load(EMemoryOrder::Acquire); }

    /**
     * 過去のピーク割当バイト数を返す。
     *
     * @return これまでの最大割当バイト数。
     */
    u64 PeakBytes()      const noexcept override { return m_Peak.Load(EMemoryOrder::Acquire); }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "System"。
     */
    const char* Name()   const noexcept override { return "System"; }

private:
    /** 現在の総割当バイト数 (実ヒープ確保量、アトミック集計)。 */
    mutable TAtomic<u64> m_Bytes {0};

    /** 過去ピークの割当バイト数 (CAS で更新)。 */
    mutable TAtomic<u64> m_Peak  {0};
};

} // namespace acs
