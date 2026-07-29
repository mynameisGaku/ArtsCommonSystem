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
constexpr usize kMaximumArenaAlignment = usize{64u} * 1024u;

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
FArenaAllocator::FPage* FArenaAllocator::AllocPage(usize Size) noexcept
{
    // sizeof(Page) + Size + 64 の usize オーバーフローを防ぐ。wrap すると過小な Total で
    // alloc し、その後の base/used 計算でページ境界を越えて書き込む (OOB)。
    if (Size > (~usize(0)) - sizeof(FPage) - 64) return nullptr;
    const usize Total = sizeof(FPage) + Size + 64;
    void* const Raw = m_Backing->Alloc(Total, alignof(FPage), FSourceLoc::Current());
    if (!Raw) return nullptr;
    auto* NewPage = static_cast<FPage*>(Raw);
    NewPage->next = nullptr;
    // データ領域はヘッダ直後を 64B 整列した位置から始める（SIMD 安全）
    NewPage->base = reinterpret_cast<u8*>(AlignUp(static_cast<u8*>(Raw) + sizeof(FPage), 64));
    NewPage->size = Size;
    NewPage->used.Store(0, EMemoryOrder::Release);
    NewPage->generation = m_Generation;
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

void* FArenaAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    return ReserveRegion(Size, Alignment, Size, 1u);
}

ACS_FORCEINLINE void* FArenaAllocator::ReserveRegion(usize ReservedSize, usize Alignment, usize AccountedBytes, usize AccountedAllocations) noexcept
{
    if (ReservedSize == 0u || AccountedAllocations == 0u) return nullptr;
    if (!IsPow2(Alignment) || Alignment > kMaximumArenaAlignment) return nullptr;
    if (ReservedSize > (~usize(0)) - (Alignment - 1u)) return nullptr;
    if (!TryBeginAllocation()) return nullptr;

    /** 関数離脱時に arena の確保入場数を戻す局所 guard。 */
    struct FActiveAllocationScope {
        /** 入場数を戻す arena。 */
        FArenaAllocator* allocator = nullptr;

        /** 保持している確保入場を終了する。 */
        ~FActiveAllocationScope() noexcept
        {
            allocator->EndAllocation();
        }
    };
    /** 現在の確保入場を所有する guard。 */
    FActiveAllocationScope AllocationScope{this};

    // 新ページの data 先頭がどこに置かれても、最大 Alignment - 1 バイトの前方余白を含めて
    // ReservedSize バイトを収容できる容量を確保する。これがないと同じ不足ページを増やし続ける。
    const usize MinimumPageSize = ReservedSize + Alignment - 1u;

    while (true) {
        FPage* CurrentPage = m_Current.Load(EMemoryOrder::Acquire);
        // m_Current は現世代へ初期化した page だけを公開し、Reset gate が操作中の世代変更を防ぐ。
        if (CurrentPage) {
            // 現在ページに収まるか確認
            const u64 CurrentUsed = CurrentPage->used.Load(EMemoryOrder::Relaxed);
            const u64 BaseAddress = reinterpret_cast<u64>(CurrentPage->base);
            if (CurrentUsed > (~u64(0)) - BaseAddress || BaseAddress + CurrentUsed > (~u64(0)) - (Alignment - 1u)) {
                return nullptr;
            }
            const u64 AlignedOffset = AlignUp(BaseAddress + CurrentUsed, Alignment) - BaseAddress;
            if (AlignedOffset <= CurrentPage->size && ReservedSize <= CurrentPage->size - AlignedOffset) {
                const u64 NextUsed = AlignedOffset + ReservedSize;
                // CAS でカーソルを進める
                u64 ExpectedUsed = CurrentUsed;
                if (CurrentPage->used.CompareExchange(ExpectedUsed, NextUsed)) {
                    // ピーク値を CAS で更新
                    const u64 AllocatedBytes = m_Bytes.FetchAdd(AccountedBytes) + AccountedBytes;
                    m_AllocationCount.FetchAdd(AccountedAllocations);
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
        FPage* CurrentPageAfterLock = m_Current.Load(EMemoryOrder::Acquire);
        if (CurrentPageAfterLock != CurrentPage) continue;

        // Reset(false) 後に未使用へ戻った既存ページを先に再公開する。
        // 保持済みページが使える限り backing から追加確保してはならない。
        for (FPage* CandidatePage = m_Pages; CandidatePage; CandidatePage = CandidatePage->next) {
            if (CandidatePage == CurrentPageAfterLock) continue;

            if (CandidatePage->generation != m_Generation) {
                CandidatePage->used.Store(0u, EMemoryOrder::Relaxed);
                CandidatePage->generation = m_Generation;
                m_LazyPageResetCount.FetchAdd(1u);
            }
            const u64 CandidateUsed = CandidatePage->used.Load(EMemoryOrder::Acquire);
            const u64 CandidateBaseAddress = reinterpret_cast<u64>(CandidatePage->base);
            if (CandidateUsed > (~u64(0)) - CandidateBaseAddress ||
                CandidateBaseAddress + CandidateUsed > (~u64(0)) - (Alignment - 1u)) {
                continue;
            }

            const u64 CandidateOffset = AlignUp(CandidateBaseAddress + CandidateUsed, Alignment) -
                                        CandidateBaseAddress;
            if (CandidateOffset <= CandidatePage->size && ReservedSize <= CandidatePage->size - CandidateOffset) {
                m_Current.Store(CandidatePage, EMemoryOrder::Release);
                break;
            }
        }
        if (m_Current.Load(EMemoryOrder::Acquire) != CurrentPageAfterLock) continue;

        // 要求サイズが m_PageSize より大きければ専用ページを作る
        const usize NewPageSize = MinimumPageSize > m_PageSize ? MinimumPageSize : m_PageSize;
        FPage* NewPage = AllocPage(NewPageSize);
        if (!NewPage) return nullptr;
        NewPage->next = m_Pages;
        m_Pages = NewPage;
        m_Current.Store(NewPage, EMemoryOrder::Release);
        // 新ページに対して Alloc を再試行
    }
}

bool FArenaAllocator::AllocBatch(void** Output, usize Count, usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (!Output || Count == 0u) return false;
    /** 失敗時の契約を先に満たす出力 index。 */
    for (usize Index = 0u; Index < Count; ++Index) Output[Index] = nullptr;
    if (Size == 0u || !IsPow2(Alignment) || Alignment > kMaximumArenaAlignment) return false;
    if (Size > (~usize{0}) - (Alignment - 1u)) return false;

    /** 隣接領域の alignment を保つ byte 間隔。 */
    const usize Stride = AlignUp(Size, Alignment);
    if (Count - 1u > ((~usize{0}) - Size) / Stride) return false;
    /** cursor 上で 1 回だけ予約する総 byte 数。 */
    const usize ReservedSize = Stride * (Count - 1u) + Size;
    if (Count > (~usize{0}) / Size) return false;
    /** 利用者へ返す padding を除いた総 byte 数。 */
    const usize AccountedBytes = Size * Count;
    /** 1 回で確保した連続領域の先頭。 */
    u8* const First = static_cast<u8*>(ReserveRegion(ReservedSize, Alignment, AccountedBytes, Count));
    if (!First) return false;

    /** 予約領域から利用者領域を切り出す index。 */
    for (usize Index = 0u; Index < Count; ++Index) Output[Index] = First + Stride * Index;
    m_BatchAllocationCount.FetchAdd(1u);
    m_BatchSuballocationCount.FetchAdd(Count);
    return true;
}

void FArenaAllocator::Free(void* /*Pointer*/) noexcept
{
    // 個別解放はサポートしない（Reset で全体破棄）
}

bool FArenaAllocator::ContainsCurrentAllocationRange(const void* Pointer, usize Size) noexcept
{
    if (!Pointer || Size == 0u)
    {
        return false;
    }

    const uptr RangeBegin = reinterpret_cast<uptr>(Pointer);
    if (RangeBegin > (~uptr{0}) - Size)
    {
        return false;
    }
    const uptr RangeEnd = RangeBegin + Size;

    if (!TryBeginAllocation())
    {
        return false;
    }

    struct FActiveOperationScope
    {
        FArenaAllocator* Allocator = nullptr;

        ~FActiveOperationScope() noexcept
        {
            Allocator->EndAllocation();
        }
    } OperationScope{this};

    FScopedLock ScopedGrowLock(m_GrowLock);
    for (FPage* CurrentPage = m_Pages; CurrentPage; CurrentPage = CurrentPage->next)
    {
        if (CurrentPage->generation != m_Generation)
        {
            continue;
        }
        const uptr PageBegin = reinterpret_cast<uptr>(CurrentPage->base);
        const u64 UsedBytes = CurrentPage->used.Load(EMemoryOrder::Acquire);
        if (UsedBytes > static_cast<u64>(~uptr{0} - PageBegin))
        {
            continue;
        }
        const uptr PageEnd = PageBegin + static_cast<uptr>(UsedBytes);
        if (RangeBegin >= PageBegin && RangeEnd <= PageEnd)
        {
            return true;
        }
    }
    return false;
}

FArenaAllocatorDiagnostics FArenaAllocator::Diagnostics() const noexcept
{
    /** page 列の安定した読み取りを保証する lock。 */
    FScopedLock ScopedGrowLock(m_GrowLock);
    /** 現在の診断 snapshot。 */
    FArenaAllocatorDiagnostics Diagnostics{};
    /** 保持数へ加算する page。 */
    for (const FPage* Page = m_Pages; Page; Page = Page->next) ++Diagnostics.retained_pages;
    Diagnostics.batch_allocations = m_BatchAllocationCount.Load(EMemoryOrder::Acquire);
    Diagnostics.batch_suballocations = m_BatchSuballocationCount.Load(EMemoryOrder::Acquire);
    Diagnostics.last_reset_page_visits = m_LastResetPageVisits.Load(EMemoryOrder::Acquire);
    Diagnostics.lazy_page_resets = m_LazyPageResetCount.Load(EMemoryOrder::Acquire);
    return Diagnostics;
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
    /** 今回の Reset が直接参照した page 数。 */
    u64 ResetPageVisits = 0u;
    /** 世代 wrap 時だけ古い世代値との衝突を消す。 */
    const bool GenerationWrapped = m_Generation == ~u64{0};
    if (GenerationWrapped && !bReleasePages) {
        /** wrap 前の世代値を除去する page。 */
        for (FPage* PageIterator = m_Pages; PageIterator; PageIterator = PageIterator->next) {
            PageIterator->generation = 0u;
            ++ResetPageVisits;
        }
    }
    m_Generation = GenerationWrapped ? 1u : m_Generation + 1u;

    if (bReleasePages) {
        // 全ページを backing に返却
        FPage* CurrentPage = m_Pages;
        while (CurrentPage) {
            FPage* const NextPage = CurrentPage->next;
            m_Backing->Free(CurrentPage);
            CurrentPage = NextPage;
            ++ResetPageVisits;
        }
        m_Pages = nullptr;
        m_Current.Store(nullptr, EMemoryOrder::Release);
    } else {
        // 先頭 page だけを即時初期化し、残りは GrowLock 内で初回利用時に初期化する。
        if (m_Pages) {
            m_Pages->used.Store(0u, EMemoryOrder::Relaxed);
            m_Pages->generation = m_Generation;
            if (!GenerationWrapped) ResetPageVisits = 1u;
        }
        m_Current.Store(m_Pages, EMemoryOrder::Release);
    }
    m_Bytes.Store(0, EMemoryOrder::Release);
    m_AllocationCount.Store(0, EMemoryOrder::Release);
    m_BatchAllocationCount.Store(0u, EMemoryOrder::Release);
    m_BatchSuballocationCount.Store(0u, EMemoryOrder::Release);
    m_LazyPageResetCount.Store(0u, EMemoryOrder::Release);
    m_LastResetPageVisits.Store(ResetPageVisits, EMemoryOrder::Release);
    m_ResetInProgress.Store(0u, EMemoryOrder::Release);
}

} // namespace acs
