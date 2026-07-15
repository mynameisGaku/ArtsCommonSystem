// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — グローバルアロケータ実装 + FAllocator::Realloc デフォルト
// -----------------------------------------------------------------------------
// CRT (memcpy/memmove/memset/memcmp) は SSE/AVX で最適化されているため、
// そのまま委譲する。自前実装するメリットは皆無に近い。
// =============================================================================
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "threading/Atomic.h"

#include <cstring>
#include <new>

namespace acs {

namespace {
/**
 * プロセス終了まで破棄しないシステムアロケータを返す。
 *
 * @details 別翻訳単位の静的オブジェクトから起動時・終了時に呼ばれても、構築順と
 * 破棄順に依存しない。OS がプロセスヒープとともに最終回収する。
 */
FSystemAllocator& ProcessSystemAllocator() noexcept
{
    alignas(FSystemAllocator) static byte Storage[sizeof(FSystemAllocator)] = {};
    static FSystemAllocator* Allocator = ::new (static_cast<void*>(Storage)) FSystemAllocator();
    return *Allocator;
}

/** 現在の既定アロケータを保持する、初回利用時構築のatomicスロット。 */
TAtomic<FAllocator*>& DefaultAllocatorSlot() noexcept
{
    static TAtomic<FAllocator*> Allocator{&ProcessSystemAllocator()};
    return Allocator;
}
} // namespace

FAllocator& DefaultAllocator() noexcept
{
    return *DefaultAllocatorSlot().Load(EMemoryOrder::Acquire);
}

void SetDefaultAllocator(FAllocator* Allocator) noexcept
{
    DefaultAllocatorSlot().Store(Allocator ? Allocator : &ProcessSystemAllocator(), EMemoryOrder::Release);
}

void MemCopy(void* Destination, const void* Source, usize Size) noexcept
{
    ::memcpy(Destination, Source, Size);
}

void MemMove(void* Destination, const void* Source, usize Size) noexcept
{
    ::memmove(Destination, Source, Size);
}

void MemSet(void* Destination, int Value, usize Size) noexcept
{
    ::memset(Destination, Value, Size);
}

int MemCmp(const void* Left, const void* Right, usize Size) noexcept
{
    return ::memcmp(Left, Right, Size);
}

/**
 * Realloc のデフォルト実装 (Alloc + MemCopy + Free)。
 *
 * @details
 * 派生アロケータが override しない場合のフォールバック。NewSize==0 なら Free して
 * nullptr を返す。新規確保に失敗したら旧領域を保持したまま nullptr を返す (旧データは無傷)。
 * 効率的な in-place realloc を持つ実装 (FTlsfAllocator 等) は override すべき。
 * @param Pointer 既存の確保 (nullptr なら新規 Alloc 相当)。
 * @param OldSize 旧サイズ (コピーするバイト数の決定に使う)。
 * @param NewSize 新サイズ (0 なら解放のみ)。
 * @param Alignment 新領域のアライメント。
 * @param Location 診断用の呼び出し位置。
 * @return 新しい確保 (失敗時や NewSize==0 のとき nullptr)。
 */
void* FAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept
{
    if (NewSize == 0) {
        Free(Pointer);
        return nullptr;
    }
    void* const NewPointer = Alloc(NewSize, Alignment, Location);
    if (!NewPointer) return nullptr;
    if (Pointer) {
        const usize CopySize = OldSize < NewSize ? OldSize : NewSize;
        MemCopy(NewPointer, Pointer, CopySize);
        Free(Pointer);
    }
    return NewPointer;
}

} // namespace acs
