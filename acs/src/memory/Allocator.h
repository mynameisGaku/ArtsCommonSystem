// SPDX-License-Identifier: Apache-2.0
// ACS Memory — アロケータ抽象インターフェイス
//
// すべてのコンテナ・スマートポインタ・サブシステムは IAllocator* を介して
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
//   薄いアダプタ越しに呼ぶか、ヒューリスティック inline 候補（CPoolAllocator
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
class IAllocator {
public:
    /** 派生アロケータを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAllocator() noexcept = default;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント (2 のべき乗)。
     * @param Location 診断用の呼び出し位置 (リーク追跡やプロファイラに渡される)。
     * @return 確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    virtual void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept = 0;

    /**
     * Pointer を解放する。
     *
     * @param Pointer 解放する領域 (nullptr は no-op)。
     */
    virtual void Free(void* Pointer) noexcept = 0;

    /**
     * 既存ポインタを NewSize に拡張・縮小する。
     *
     * @details 既定実装は Alloc + memcpy + Free (Memory.cpp で定義)。
     * @param Pointer 既存の確保領域 (nullptr なら新規確保扱い)。
     * @param OldSize Pointer の現在のバイト数。
     * @param NewSize 確保し直す新しいバイト数。
     * @param Alignment 要求アライメント (2 のべき乗)。
     * @param Location 診断用の呼び出し位置。
     * @return 再確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    virtual void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept;

    /**
     * 現在の使用バイト数を返す。
     *
     * @return 現在の確保済みバイト数 (既定は 0。実装側で上書き)。
     */
    virtual u64 BytesAllocated() const noexcept
    {
        return 0;
    }

    /**
     * 現在生存している確保の件数を返す。
     *
     * @details アロケータが件数追跡を提供しない場合は 0 を返す。
     * @return 呼び出し側がまだ解放していない確保の件数。
     */
    virtual u64 AllocationCount() const noexcept
    {
        return 0;
    }

    /**
     * ピーク使用バイト数を返す。
     *
     * @return これまでの最大確保済みバイト数 (既定は 0。実装側で上書き)。
     */
    virtual u64 PeakBytes() const noexcept
    {
        return 0;
    }

    /**
     * 現在利用できるアロケータ寿命の世代を返す。
     *
     * @details
     * backing allocator を長寿命アダプタへ結び付ける際、同一アドレスの再利用や
     * Shutdown/再初期化を別寿命として識別するために使う。0 は世代追跡を提供しない、
     * または現在無効な寿命を表す。寿命管理を提供する実装は有効期間ごとに異なる非0値を返す。
     * @return 現在の寿命世代。追跡非対応または無効なら0。
     */
    virtual u64 LifetimeGeneration() const noexcept
    {
        return 0;
    }

    /**
     * アロケータの識別名を返す。
     *
     * @return アロケータ名 (既定は "IAllocator"。実装側で上書き)。
     */
    virtual const char* Name() const noexcept
    {
        return "IAllocator";
    }

    /**
     * 既定アライメントで Size バイトを確保する利便性オーバーロード。
     *
     * @param Size 確保するバイト数。
     * @param Location 診断用の呼び出し位置 (既定で呼び出し元を自動キャプチャ)。
     * @return 確保した領域の先頭ポインタ。失敗時は nullptr。
     */
    ACS_FORCEINLINE void* Alloc(usize Size, FSourceLoc Location = FSourceLoc::Current()) noexcept
    {
        return Alloc(Size, kDefaultAlignment, Location);
    }
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FAllocator = IAllocator;

/**
 * Value が 2 のべき乗かを判定する。
 *
 * @param Value 判定する値。
 * @return 2 のべき乗 (0 は除く) なら true。
 */
ACS_FORCEINLINE bool IsPow2(usize Value) noexcept
{
    return Value != 0 && (Value & (Value - 1)) == 0;
}

/**
 * Value を Alignment の倍数に切り上げる。
 *
 * @details Alignment は 2 のべき乗である必要があり、(Value + Alignment-1) がラップしないことを assert する。
 * @param Value 切り上げる値。
 * @param Alignment アライメント (2 のべき乗)。
 * @return Alignment の倍数に切り上げた値。
 */
ACS_FORCEINLINE usize AlignUp(usize Value, usize Alignment) noexcept
{
    ACS_ASSERT(Alignment != 0 && (Alignment & (Alignment - 1)) == 0); // Alignment は 2 のべき乗である必要
    ACS_ASSERT(Value <= (~usize(0)) - (Alignment - 1));               // 加算がラップしてはならない
    return (Value + (Alignment - 1)) & ~(Alignment - 1);
}

/**
 * Pointer を Alignment の倍数アドレスに切り上げる。
 *
 * @param Pointer 切り上げるポインタ。
 * @param Alignment アライメント (2 のべき乗)。
 * @return Alignment 整列に切り上げたポインタ。
 */
ACS_FORCEINLINE void* AlignUp(void* Pointer, usize Alignment) noexcept
{
    return reinterpret_cast<void*>(AlignUp(reinterpret_cast<uptr>(Pointer), Alignment));
}

} // namespace acs
