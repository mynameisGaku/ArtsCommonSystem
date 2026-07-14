// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FArenaAllocator 実装
// -----------------------------------------------------------------------------
// 通常確保はアトミック CAS 1 回で完了する。ページが満杯になった時だけ
// FMutex で排他して新ページを確保する。
// =============================================================================
#include "memory/ArenaAllocator.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"
#include "threading/Thread.h"

namespace acs {

namespace {

/** arena が直接保証する最大アライメント。Windows の仮想メモリ割り当て粒度に合わせる。 */
constexpr usize kMaximumArenaAlignment = 64u * 1024u;

/** 短い待機は CPU ヒント、長引いた場合はスケジューラへ実行権を譲る。 */
void WaitForArenaOperation(u32& SpinCount) noexcept
{
    if (SpinCount < 64u) {
        ++SpinCount;
        SpinHint();
        return;
    }
    Yield();
}

} // namespace

FArenaAllocator::FArenaAllocator(usize PageSize, FAllocator* BackingAllocator) noexcept
    : m_Backing(BackingAllocator ? BackingAllocator : &DefaultAllocator()), m_PageSize(PageSize)
{
}

FArenaAllocator::~FArenaAllocator() noexcept
{
    Reset(/*release*/ true);
}

// 新ページ確保（ヘッダ + データ + 64B 整列の余裕を 1 回で取る）
FArenaAllocator::Page* FArenaAllocator::AllocPage(usize Size) noexcept
{
    // sizeof(Page) + Size + 64 の usize オーバーフローを防ぐ。wrap すると過小な Total で
    // alloc し、その後の base/used 計算でページ境界を越えて書き込む (OOB)。
    if (Size > (~usize(0)) - sizeof(Page) - 64) return nullptr;
    const usize Total = sizeof(Page) + Size + 64;
    void* const Raw = m_Backing->Alloc(Total, alignof(Page), FSourceLoc::Current());
    if (!Raw) return nullptr;
    auto* NewPage = static_cast<Page*>(Raw);
    NewPage->next = nullptr;
    // データ領域はヘッダ直後を 64B 整列した位置から始める（SIMD 安全）
    NewPage->base = reinterpret_cast<u8*>(AlignUp(static_cast<u8*>(Raw) + sizeof(Page), 64));
    NewPage->size = Size;
    NewPage->used.Store(0, EMemoryOrder::Release);
    return NewPage;
}

bool FArenaAllocator::TryBeginAllocation() noexcept
{
    if (m_ResetInProgress.Load(EMemoryOrder::Acquire) != 0u) {
        return false;
    }

    m_ActiveAllocations.FetchAdd(1u);
    if (m_ResetInProgress.Load(EMemoryOrder::Acquire) == 0u) {
        return true;
    }

    // 最初の確認直後に Reset が始まった場合は、ページへ触れる前に入場を取り消す。
    m_ActiveAllocations.FetchSub(1u);
    return false;
}

void FArenaAllocator::EndAllocation() noexcept
{
    m_ActiveAllocations.FetchSub(1u);
}

// 確保
void* FArenaAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0) return nullptr;
    if (!IsPow2(Alignment) || Alignment > kMaximumArenaAlignment) return nullptr;
    if (Size > (~usize(0)) - (Alignment - 1u)) return nullptr;
    if (!TryBeginAllocation()) return nullptr;

    struct ActiveAllocationScope {
        FArenaAllocator* allocator = nullptr;

        ~ActiveAllocationScope() noexcept
        {
            allocator->EndAllocation();
        }
    } AllocationScope{this};

    // 新ページの data 先頭がどこに置かれても、最大 Alignment - 1 バイトの前方余白を含めて
    // Size バイトを収容できる容量を確保する。これがないと同じ不足ページを増やし続ける。
    const usize MinimumPageSize = Size + Alignment - 1u;

    while (true) {
        Page* CurrentPage = m_Current.Load(EMemoryOrder::Acquire);
        if (CurrentPage) {
            // 現在ページに収まるか確認
            const u64 CurrentUsed = CurrentPage->used.Load(EMemoryOrder::Relaxed);
            const u64 BaseAddress = reinterpret_cast<u64>(CurrentPage->base);
            if (CurrentUsed > (~u64(0)) - BaseAddress || BaseAddress + CurrentUsed > (~u64(0)) - (Alignment - 1u)) {
                return nullptr;
            }
            const u64 AlignedOffset = AlignUp(BaseAddress + CurrentUsed, Alignment) - BaseAddress;
            if (AlignedOffset <= CurrentPage->size && Size <= CurrentPage->size - AlignedOffset) {
                const u64 NextUsed = AlignedOffset + Size;
                // CAS でカーソルを進める
                u64 ExpectedUsed = CurrentUsed;
                if (CurrentPage->used.CompareExchange(ExpectedUsed, NextUsed)) {
                    // ピーク値を CAS で更新
                    const u64 AllocatedBytes = m_Bytes.FetchAdd(Size) + Size;
                    m_AllocationCount.FetchAdd(1u);
                    u64 RecordedPeakBytes = m_Peak.Load(EMemoryOrder::Relaxed);
                    while (AllocatedBytes > RecordedPeakBytes &&
                           !m_Peak.CompareExchange(RecordedPeakBytes, AllocatedBytes)) {}
                    return CurrentPage->base + AlignedOffset;
                }
                continue; // 競合 — 同ページで再試行
            }
        }

        // 新ページが必要 — Grow ロックを取る
        FScopedLock ScopedGrowLock(m_GrowLock);
        // ロック取得中に他スレッドが既に Grow している可能性をチェック
        Page* CurrentPageAfterLock = m_Current.Load(EMemoryOrder::Acquire);
        if (CurrentPageAfterLock != CurrentPage) continue;
        // 要求サイズが m_PageSize より大きければ専用ページを作る
        const usize NewPageSize = MinimumPageSize > m_PageSize ? MinimumPageSize : m_PageSize;
        Page* NewPage = AllocPage(NewPageSize);
        if (!NewPage) return nullptr;
        NewPage->next = m_Pages;
        m_Pages = NewPage;
        m_Current.Store(NewPage, EMemoryOrder::Release);
        // 新ページに対して Alloc を再試行
    }
}

void FArenaAllocator::Free(void* /*Pointer*/) noexcept
{
    // 個別解放はサポートしない（Reset で全体破棄）
}

// 巻き戻し or 全解放
void FArenaAllocator::Reset(bool bReleasePages) noexcept
{
    // 複数の Reset は順番に実行する。入場ゲートを先に閉じることで GrowLock 待ちとのデッドロックを避ける。
    u32 SpinCount = 0u;
    for (;;) {
        u32 ExpectedResetState = 0u;
        if (m_ResetInProgress.CompareExchange(ExpectedResetState, 1u)) {
            break;
        }
        WaitForArenaOperation(SpinCount);
    }

    SpinCount = 0u;
    while (m_ActiveAllocations.Load(EMemoryOrder::Acquire) != 0u) {
        WaitForArenaOperation(SpinCount);
    }

    FScopedLock ScopedGrowLock(m_GrowLock);
    if (bReleasePages) {
        // 全ページを backing に返却
        Page* CurrentPage = m_Pages;
        while (CurrentPage) {
            Page* const NextPage = CurrentPage->next;
            m_Backing->Free(CurrentPage);
            CurrentPage = NextPage;
        }
        m_Pages = nullptr;
        m_Current.Store(nullptr, EMemoryOrder::Release);
    } else {
        // ページを保持してカーソルだけ巻き戻し
        for (Page* PageIterator = m_Pages; PageIterator; PageIterator = PageIterator->next)
            PageIterator->used.Store(0, EMemoryOrder::Release);
        m_Current.Store(m_Pages, EMemoryOrder::Release);
    }
    m_Bytes.Store(0, EMemoryOrder::Release);
    m_AllocationCount.Store(0, EMemoryOrder::Release);
    m_ResetInProgress.Store(0u, EMemoryOrder::Release);
}

} // namespace acs
