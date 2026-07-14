// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySystem 実装
// -----------------------------------------------------------------------------
// 通常セグメントは mimalloc first-class heap、フレーム一括寿命は arena を使う。
// 「現在のセグメント」は TLS 変数で管理し、ScopedMemorySegment で push/pop。
// =============================================================================
#include "memory/MemorySystem.h"

#include "foundation/Log.h"
#include "memory/ArenaAllocator.h"
#include "memory/MimallocAllocator.h"
#include "memory/SystemAllocator.h"
#include "memory/Memory.h" // DefaultAllocator / SetDefaultAllocator
#include "foundation/Platform.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"
#include "threading/Thread.h"

#ifndef ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE
#    define ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE 0
#endif

namespace acs {

/** 現在のスレッドが選択しているセグメント (ScopedMemorySegment で切替)。 */
ACS_THREAD_LOCAL ESegment tls_current_segment = ESegment::Default;

namespace {

/** frame(arena) セグメントのページ確保元。既定差し替え (SetDefaultAllocator) の影響を受けない独立 system allocator (再帰回避)。 */
FSystemAllocator g_frame_backing;

/** 1 セグメント分のアロケータホルダ。 */
struct SegmentSlot {
    /** このスロットのセグメント種別。 */
    ESegment segment = ESegment::Default;

    /** true ならフレーム/スクラッチ用途 (arena)、false なら汎用 (mimalloc)。 */
    bool use_frame_allocator = false;

    /** スロットが初期化済みか。 */
    bool initialized = false;

    /** 汎用アロケータ (use_frame_allocator=false のとき使用)。 */
    FMimallocAllocator mimalloc;

    /** フレーム/スクラッチ用 arena (use_frame_allocator=true のとき placement-new で確保)。 */
    FArenaAllocator* arena = nullptr;

    /** ハード予算 (バイト、0 で無制限)。 */
    u64 hard_budget_bytes = 0;

    /** frame arena の確保前にCAS予約する要求バイト合計。Resetまで個別解放しない。 */
    TAtomic<u64> frame_reserved_bytes{0};

    /** 1 の間は ResetTemp が予算・arena・追跡表を一括更新中。 */
    TAtomic<u32> frame_reset_in_progress{0};

    /** ResetTemp が完了を待つ、入場済み Temp Alloc/Free/Realloc の件数。 */
    TAtomic<u32> frame_active_operations{0};
};

#ifndef ACS_MEMORY_TRACK_ALLOCATION_SITES
#    define ACS_MEMORY_TRACK_ALLOCATION_SITES ACS_BUILD_DEBUG
#endif

#if ACS_MEMORY_TRACK_ALLOCATION_SITES

/** 追跡表のスロット状態。空と削除済みを分け、open addressing の探索を維持する。 */
enum class ETrackingSlotState : u8 {
    Empty,
    Occupied,
    Deleted,
};

/** MemorySystem 自身へ再帰確保しない、デバッグ専用の割り当て元記録。 */
struct TrackedAllocationRecord {
    void* address = nullptr;
    u64 requested_bytes = 0;
    u64 alignment = 0;
    u64 allocation_sequence = 0;
    ESegment segment = ESegment::Default;
    const char* source_file = "";
    const char* source_function = "";
    u32 source_line = 0;
    u32 source_column = 0;
    ETrackingSlotState state = ETrackingSlotState::Empty;
};

/** プロセスヒープ上の追跡表。対象アロケータを一切使わない。 */
struct AllocationTrackingState {
    SRWLOCK lock = SRWLOCK_INIT;
    TrackedAllocationRecord* records = nullptr;
    usize capacity = 0;
    usize occupied_count = 0;
    usize deleted_count = 0;
    u64 newest_allocation_sequence = 0;
    u64 dropped_tracking_event_count = 0;
    u64 break_on_allocation_sequence = 0;
    bool active = false;
};

AllocationTrackingState g_allocation_tracking;

/** 診断ログ自身の確保を同じスレッドで再追跡しないためのガード。 */
ACS_THREAD_LOCAL bool tls_ignore_allocation_tracking = false;

static constexpr usize kInitialTrackingCapacity = 1024;

/** ポインタ値を open-addressing 用に混ぜる。 */
usize HashTrackedAddress(const void* Address) noexcept
{
    u64 Value = static_cast<u64>(reinterpret_cast<uptr>(Address)) >> 4u;
    Value ^= Value >> 33u;
    Value *= 0xff51afd7ed558ccduLL;
    Value ^= Value >> 33u;
    Value *= 0xc4ceb9fe1a85ec53uLL;
    Value ^= Value >> 33u;
    return static_cast<usize>(Value);
}

TrackedAllocationRecord* AllocateTrackingTable(usize Capacity) noexcept
{
    if (Capacity == 0 || Capacity > (~usize(0) / sizeof(TrackedAllocationRecord))) {
        return nullptr;
    }
    return static_cast<TrackedAllocationRecord*>(
        ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, Capacity * sizeof(TrackedAllocationRecord)));
}

void DestroyTrackingTable(TrackedAllocationRecord* Records) noexcept
{
    if (Records) ::HeapFree(::GetProcessHeap(), 0, Records);
}

/** 指定アドレスの記録位置を探す。未登録なら capacity を返す。呼び出し側が lock を保持する。 */
usize FindTrackedRecordLocked(const void* Address) noexcept
{
    if (!Address || !g_allocation_tracking.records || g_allocation_tracking.capacity == 0) {
        return g_allocation_tracking.capacity;
    }
    const usize Mask = g_allocation_tracking.capacity - 1u;
    usize Index = HashTrackedAddress(Address) & Mask;
    for (usize Probe = 0; Probe < g_allocation_tracking.capacity; ++Probe) {
        const TrackedAllocationRecord& Record = g_allocation_tracking.records[Index];
        if (Record.state == ETrackingSlotState::Empty) return g_allocation_tracking.capacity;
        if (Record.state == ETrackingSlotState::Occupied && Record.address == Address) return Index;
        Index = (Index + 1u) & Mask;
    }
    return g_allocation_tracking.capacity;
}

/** 新規記録位置を探す。削除済みスロットは最初の候補として再利用する。 */
usize FindTrackingInsertionSlotLocked(const void* Address) noexcept
{
    const usize Mask = g_allocation_tracking.capacity - 1u;
    usize Index = HashTrackedAddress(Address) & Mask;
    usize FirstDeleted = g_allocation_tracking.capacity;
    for (usize Probe = 0; Probe < g_allocation_tracking.capacity; ++Probe) {
        const TrackedAllocationRecord& Record = g_allocation_tracking.records[Index];
        if (Record.state == ETrackingSlotState::Empty) {
            return FirstDeleted != g_allocation_tracking.capacity ? FirstDeleted : Index;
        }
        if (Record.state == ETrackingSlotState::Deleted && FirstDeleted == g_allocation_tracking.capacity) {
            FirstDeleted = Index;
        } else if (Record.state == ETrackingSlotState::Occupied && Record.address == Address) {
            return Index;
        }
        Index = (Index + 1u) & Mask;
    }
    return FirstDeleted;
}

/** 追跡表を power-of-two 容量へ拡張し、既存記録を再配置する。 */
bool ResizeTrackingTableLocked(usize NewCapacity) noexcept
{
    TrackedAllocationRecord* const Replacement = AllocateTrackingTable(NewCapacity);
    if (!Replacement) return false;

    TrackedAllocationRecord* const PreviousRecords = g_allocation_tracking.records;
    const usize PreviousCapacity = g_allocation_tracking.capacity;
    g_allocation_tracking.records = Replacement;
    g_allocation_tracking.capacity = NewCapacity;
    g_allocation_tracking.occupied_count = 0;
    g_allocation_tracking.deleted_count = 0;

    for (usize i = 0; i < PreviousCapacity; ++i) {
        const TrackedAllocationRecord& Source = PreviousRecords[i];
        if (Source.state != ETrackingSlotState::Occupied) continue;
        const usize DestinationIndex = FindTrackingInsertionSlotLocked(Source.address);
        if (DestinationIndex == g_allocation_tracking.capacity) {
            ++g_allocation_tracking.dropped_tracking_event_count;
            continue;
        }
        g_allocation_tracking.records[DestinationIndex] = Source;
        ++g_allocation_tracking.occupied_count;
    }
    DestroyTrackingTable(PreviousRecords);
    return true;
}

bool EnsureTrackingCapacityLocked() noexcept
{
    if (g_allocation_tracking.capacity == 0) {
        return ResizeTrackingTableLocked(kInitialTrackingCapacity);
    }
    const usize UsedSlots = g_allocation_tracking.occupied_count + g_allocation_tracking.deleted_count;
    if ((UsedSlots + 1u) * 10u < g_allocation_tracking.capacity * 7u) return true;
    if (g_allocation_tracking.capacity > (~usize(0) / 2u)) return false;
    return ResizeTrackingTableLocked(g_allocation_tracking.capacity * 2u);
}

void BeginAllocationTrackingLifecycle() noexcept
{
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    DestroyTrackingTable(g_allocation_tracking.records);
    g_allocation_tracking.records = nullptr;
    g_allocation_tracking.capacity = 0;
    g_allocation_tracking.occupied_count = 0;
    g_allocation_tracking.deleted_count = 0;
    g_allocation_tracking.newest_allocation_sequence = 0;
    g_allocation_tracking.dropped_tracking_event_count = 0;
    g_allocation_tracking.active = true;
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);
}

void EndAllocationTrackingLifecycle() noexcept
{
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    g_allocation_tracking.active = false;
    DestroyTrackingTable(g_allocation_tracking.records);
    g_allocation_tracking.records = nullptr;
    g_allocation_tracking.capacity = 0;
    g_allocation_tracking.occupied_count = 0;
    g_allocation_tracking.deleted_count = 0;
    g_allocation_tracking.newest_allocation_sequence = 0;
    g_allocation_tracking.dropped_tracking_event_count = 0;
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);
}

void TrackAllocation(void* Address, usize RequestedBytes, usize Alignment, ESegment Segment,
                     FSourceLoc SourceLocation) noexcept
{
    if (!Address || tls_ignore_allocation_tracking) return;

    u64 AssignedSequence = 0;
    u64 BreakSequence = 0;
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    if (g_allocation_tracking.active) {
        AssignedSequence = ++g_allocation_tracking.newest_allocation_sequence;
        if (!EnsureTrackingCapacityLocked()) {
            ++g_allocation_tracking.dropped_tracking_event_count;
        } else {
            const usize Index = FindTrackingInsertionSlotLocked(Address);
            if (Index == g_allocation_tracking.capacity) {
                ++g_allocation_tracking.dropped_tracking_event_count;
            } else {
                TrackedAllocationRecord& Record = g_allocation_tracking.records[Index];
                if (Record.state != ETrackingSlotState::Occupied) {
                    if (Record.state == ETrackingSlotState::Deleted) {
                        --g_allocation_tracking.deleted_count;
                    }
                    ++g_allocation_tracking.occupied_count;
                } else {
                    ++g_allocation_tracking.dropped_tracking_event_count;
                }
                Record.address = Address;
                Record.requested_bytes = static_cast<u64>(RequestedBytes);
                Record.alignment = static_cast<u64>(Alignment);
                Record.allocation_sequence = AssignedSequence;
                Record.segment = Segment;
                Record.source_file = SourceLocation.File();
                Record.source_function = SourceLocation.Function();
                Record.source_line = SourceLocation.Line();
                Record.source_column = SourceLocation.Column();
                Record.state = ETrackingSlotState::Occupied;
            }
        }
        BreakSequence = g_allocation_tracking.break_on_allocation_sequence;
    }
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);

    if (AssignedSequence != 0 && AssignedSequence == BreakSequence && ::IsDebuggerPresent()) {
        ::OutputDebugStringA("[acs][memory] break_on_allocation_sequence matched\n");
        ACS_DEBUGBREAK();
    }
}

void UntrackAllocation(void* Address) noexcept
{
    if (!Address || tls_ignore_allocation_tracking) return;
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    if (g_allocation_tracking.active && g_allocation_tracking.capacity != 0) {
        const usize Index = FindTrackedRecordLocked(Address);
        if (Index == g_allocation_tracking.capacity) {
            ++g_allocation_tracking.dropped_tracking_event_count;
        } else {
            TrackedAllocationRecord& Record = g_allocation_tracking.records[Index];
            Record.address = nullptr;
            Record.state = ETrackingSlotState::Deleted;
            --g_allocation_tracking.occupied_count;
            ++g_allocation_tracking.deleted_count;
        }
    }
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);
}

void RetrackAllocation(void* PreviousAddress, void* NewAddress, usize RequestedBytes, usize Alignment, ESegment Segment,
                       FSourceLoc SourceLocation) noexcept
{
    if (tls_ignore_allocation_tracking) return;
    if (PreviousAddress && PreviousAddress != NewAddress) UntrackAllocation(PreviousAddress);
    if (!NewAddress) return;
    if (PreviousAddress == NewAddress) UntrackAllocation(PreviousAddress);
    TrackAllocation(NewAddress, RequestedBytes, Alignment, Segment, SourceLocation);
}

void ForgetTrackedSegment(ESegment Segment) noexcept
{
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    if (g_allocation_tracking.active) {
        for (usize i = 0; i < g_allocation_tracking.capacity; ++i) {
            TrackedAllocationRecord& Record = g_allocation_tracking.records[i];
            if (Record.state != ETrackingSlotState::Occupied || Record.segment != Segment) continue;
            Record.address = nullptr;
            Record.state = ETrackingSlotState::Deleted;
            --g_allocation_tracking.occupied_count;
            ++g_allocation_tracking.deleted_count;
        }
    }
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);
}

MemoryTrackingReport CollectTrackedAllocations(MemoryTrackingCheckpoint Checkpoint, OutstandingMemoryAllocation* Output,
                                               u32 OutputCapacity) noexcept
{
    MemoryTrackingReport Report;
    ::AcquireSRWLockShared(&g_allocation_tracking.lock);
    Report.tracking_enabled = g_allocation_tracking.active;
    Report.newest_allocation_sequence = g_allocation_tracking.newest_allocation_sequence;
    Report.dropped_tracking_event_count = g_allocation_tracking.dropped_tracking_event_count;
    if (g_allocation_tracking.active) {
        for (usize i = 0; i < g_allocation_tracking.capacity; ++i) {
            const TrackedAllocationRecord& Record = g_allocation_tracking.records[i];
            if (Record.state != ETrackingSlotState::Occupied ||
                Record.allocation_sequence <= Checkpoint.allocation_sequence) {
                continue;
            }
            ++Report.outstanding_allocation_count;
            Report.outstanding_requested_bytes += Record.requested_bytes;
            if (!Output || Report.written_allocation_count >= OutputCapacity) continue;
            OutstandingMemoryAllocation& Destination = Output[Report.written_allocation_count++];
            Destination.address = Record.address;
            Destination.requested_bytes = Record.requested_bytes;
            Destination.alignment = Record.alignment;
            Destination.allocation_sequence = Record.allocation_sequence;
            Destination.segment = Record.segment;
            Destination.source_file = Record.source_file;
            Destination.source_function = Record.source_function;
            Destination.source_line = Record.source_line;
            Destination.source_column = Record.source_column;
        }
    }
    ::ReleaseSRWLockShared(&g_allocation_tracking.lock);
    return Report;
}

#else

ACS_THREAD_LOCAL bool tls_ignore_allocation_tracking = false;

void BeginAllocationTrackingLifecycle() noexcept
{
}
void EndAllocationTrackingLifecycle() noexcept
{
}
void TrackAllocation(void*, usize, usize, ESegment, FSourceLoc) noexcept
{
}
void UntrackAllocation(void*) noexcept
{
}
void RetrackAllocation(void*, void*, usize, usize, ESegment, FSourceLoc) noexcept
{
}
void ForgetTrackedSegment(ESegment) noexcept
{
}
MemoryTrackingReport CollectTrackedAllocations(MemoryTrackingCheckpoint, OutstandingMemoryAllocation*, u32) noexcept
{
    return {};
}

#endif

/** SegmentAllocator の有効寿命へプロセス内で一意な非0世代を払い出す。 */
TAtomic<u64> g_segment_allocator_lifetime_generation{0};

/** Temp 操作の短い待機では CPU ヒント、長引いた場合はスケジューラへ実行権を譲る。 */
void WaitForFrameOperation(u32& SpinCount) noexcept
{
    if (SpinCount < 64u) {
        ++SpinCount;
        SpinHint();
        return;
    }
    Yield();
}

/** ResetTemp と競合しない Temp 操作として入場できれば true を返す。 */
bool TryBeginFrameOperation(SegmentSlot& Slot) noexcept
{
    if (Slot.frame_reset_in_progress.Load(EMemoryOrder::Acquire) != 0u) {
        return false;
    }

    Slot.frame_active_operations.FetchAdd(1u);
    if (Slot.frame_reset_in_progress.Load(EMemoryOrder::Acquire) == 0u) {
        return true;
    }

    // 最初の確認直後に ResetTemp が始まった場合は、予算や追跡表へ触れる前に入場を取り消す。
    Slot.frame_active_operations.FetchSub(1u);
    return false;
}

/** Temp 操作の完了を ResetTemp へ通知する。 */
void EndFrameOperation(SegmentSlot& Slot) noexcept
{
    Slot.frame_active_operations.FetchSub(1u);
}

/** ResetTemp の入場権を直列化し、開始済み Temp 操作がすべて終わるまで待つ。 */
void BeginFrameReset(SegmentSlot& Slot) noexcept
{
    u32 SpinCount = 0u;
    for (;;) {
        u32 Expected = 0u;
        if (Slot.frame_reset_in_progress.CompareExchange(Expected, 1u)) {
            break;
        }
        WaitForFrameOperation(SpinCount);
    }

    SpinCount = 0u;
    while (Slot.frame_active_operations.Load(EMemoryOrder::Acquire) != 0u) {
        WaitForFrameOperation(SpinCount);
    }
}

/** ResetTemp 完了後に Temp 操作の入場を再開する。 */
void EndFrameReset(SegmentSlot& Slot) noexcept
{
    Slot.frame_reset_in_progress.Store(0u, EMemoryOrder::Release);
}

/** SegmentAllocator の Temp 操作を関数スコープ全体で登録する。 */
class FrameOperationScope final {
public:
    explicit FrameOperationScope(SegmentSlot* Slot) noexcept : m_Slot(Slot)
    {
        if (!m_Slot || !m_Slot->use_frame_allocator) {
            m_bAdmitted = true;
            return;
        }
        m_bAdmitted = TryBeginFrameOperation(*m_Slot);
        m_bRegistered = m_bAdmitted;
    }

    ~FrameOperationScope() noexcept
    {
        if (m_bRegistered) {
            EndFrameOperation(*m_Slot);
        }
    }

    FrameOperationScope(const FrameOperationScope&) = delete;
    FrameOperationScope& operator=(const FrameOperationScope&) = delete;

    /** ResetTemp と競合せず、操作を続行できるなら true。 */
    bool WasAdmitted() const noexcept
    {
        return m_bAdmitted;
    }

private:
    /** 操作対象スロット。 */
    SegmentSlot* m_Slot = nullptr;

    /** 操作を続行できる場合は true。 */
    bool m_bAdmitted = false;

    /** frame_active_operations へ登録済みなら true。 */
    bool m_bRegistered = false;
};

/**
 * SegmentSlot を FAllocator インターフェイスとして公開するアダプタ。
 *
 * @details
 * 裏側の mimalloc / arena はどちらも内部でスレッド安全なので、ここではロックを取らない。
 * mimalloc と frame arena のハード予算は、どちらも確保中の予約量を含む CAS で厳密に守る。
 * arena は ResetTemp まで個別解放しないため、再確保時も新しい要求量を追加予約する。
 */
class SegmentAllocator final : public FAllocator {
public:
    /** 未バインド状態で構築する (使用前に Bind を呼ぶこと)。 */
    SegmentAllocator() noexcept = default;

    /**
     * 公開対象の SegmentSlot を結び付ける。
     *
     * @param Slot このアダプタが委譲する先のスロット。
     */
    void Bind(SegmentSlot* Slot) noexcept
    {
        m_Slot = Slot;
        u64 Generation = g_segment_allocator_lifetime_generation.FetchAdd(1u) + 1u;
        if (Generation == 0u) {
            Generation = g_segment_allocator_lifetime_generation.FetchAdd(1u) + 1u;
        }
        m_LifetimeGeneration.Store(Generation, EMemoryOrder::Release);
    }

    /** 現在の結び付き寿命を無効化し、遅延解放が backing へ到達しないようにする。 */
    void InvalidateLifetime() noexcept
    {
        m_LifetimeGeneration.Store(0u, EMemoryOrder::Release);
    }

    /**
     * 予算チェック後に裏側アロケータへ確保を委譲する。
     *
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント。
     * @param Location 診断用の呼び出し位置。
     * @return 確保した領域 (未初期化/予算超過/失敗時は nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (!m_Slot || !m_Slot->initialized) {
            return nullptr;
        }
        FrameOperationScope FrameOperation(m_Slot);
        if (!FrameOperation.WasAdmitted()) {
            return nullptr;
        }
        return AllocateFromBacking(Size, Alignment, Location);
    }

    /**
     * 裏側アロケータへ解放を委譲する。
     *
     * @param Pointer 解放する領域 (未初期化/nullptr は no-op)。
     */
    void Free(void* Pointer) noexcept override
    {
        if (!m_Slot || !m_Slot->initialized || !Pointer) {
            return;
        }
        FrameOperationScope FrameOperation(m_Slot);
        if (!FrameOperation.WasAdmitted()) {
            return;
        }

        // 通常セグメントでは、別ヒープや内部ポインタで追跡記録だけが消えるのを防ぐ。
        if (!m_Slot->use_frame_allocator && !m_Slot->mimalloc.OwnsAllocation(Pointer)) {
            return;
        }
        UntrackAllocation(Pointer);
        Backing()->Free(Pointer);
    }

    /**
     * 裏側アロケータへ再確保を委譲する。
     *
     * @param Pointer 既存の確保。
     * @param OldSize 旧サイズ。
     * @param NewSize 新サイズ。
     * @param Alignment 要求アライメント。
     * @param Location 診断用の呼び出し位置。
     * @return 再確保した領域 (未初期化/失敗時は nullptr)。
     */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (!m_Slot || !m_Slot->initialized) {
            return nullptr;
        }
        FrameOperationScope FrameOperation(m_Slot);
        if (!FrameOperation.WasAdmitted()) {
            return nullptr;
        }

        if (m_Slot->use_frame_allocator) {
            if (!Pointer) {
                return AllocateFromBacking(NewSize, Alignment, Location);
            }
            if (NewSize == 0) {
                UntrackAllocation(Pointer);
                return nullptr;
            }

            void* const Allocation = AllocateFromBacking(NewSize, Alignment, Location);
            if (!Allocation) {
                return nullptr;
            }
            MemCopy(Allocation, Pointer, OldSize < NewSize ? OldSize : NewSize);
            UntrackAllocation(Pointer);
            return Allocation;
        }

        // Realloc(正しくないポインタ, 0) でも追跡記録を削除してはならない。
        if (Pointer && !m_Slot->mimalloc.OwnsAllocation(Pointer)) {
            return nullptr;
        }

        void* const Allocation = Backing()->Realloc(Pointer, OldSize, NewSize, Alignment, Location);
        if (Allocation || NewSize == 0) {
            RetrackAllocation(Pointer, Allocation, NewSize, Alignment, m_Slot->segment, Location);
        }
        return Allocation;
    }

    /**
     * 裏側アロケータの現在使用バイト数を返す。
     *
     * @return 使用バイト数 (未初期化なら 0)。
     */
    u64 BytesAllocated() const noexcept override
    {
        return (m_Slot && m_Slot->initialized) ? Backing()->BytesAllocated() : 0;
    }

    /**
     * 裏側アロケータが数える現在の未解放割り当て件数を返す。
     *
     * @return 未解放件数 (未初期化なら 0)。
     */
    u64 AllocationCount() const noexcept override
    {
        return (m_Slot && m_Slot->initialized) ? Backing()->AllocationCount() : 0;
    }

    /**
     * 裏側アロケータのピークバイト数を返す。
     *
     * @return ピークバイト数 (未初期化なら 0)。
     */
    u64 PeakBytes() const noexcept override
    {
        return (m_Slot && m_Slot->initialized) ? Backing()->PeakBytes() : 0;
    }

    /** MemorySystem の初期化寿命ごとに異なる世代を返す。 */
    u64 LifetimeGeneration() const noexcept override
    {
        return m_LifetimeGeneration.Load(EMemoryOrder::Acquire);
    }

    /**
     * セグメント名を返す。
     *
     * @return バインド先セグメント名 (未バインドなら "Unbound")。
     */
    const char* Name() const noexcept override
    {
        return m_Slot ? ToString(m_Slot->segment) : "Unbound";
    }

private:
    /** 入場済み操作として予算予約・具象確保・追跡登録を一続きで行う。 */
    void* AllocateFromBacking(usize Size, usize Alignment, FSourceLoc Location) noexcept
    {
        FAllocator* const Allocator = Backing();
        const bool bFrameBudgetReserved = TryReserveFrameBudget(Size);
        if (!bFrameBudgetReserved) {
            return nullptr;
        }
        void* const Allocation = Allocator->Alloc(Size, Alignment, Location);
        if (!Allocation) {
            RollBackFrameBudget(Size);
        }
        TrackAllocation(Allocation, Size, Alignment, m_Slot->segment, Location);
        return Allocation;
    }

    /** frame arena の要求量をハード予算内でCAS予約する。通常ヒープと無制限設定は何もしない。 */
    bool TryReserveFrameBudget(usize Size) noexcept
    {
        if (!m_Slot->use_frame_allocator || m_Slot->hard_budget_bytes == 0u) {
            return true;
        }

        const u64 RequestedBytes = static_cast<u64>(Size);
        u64 Current = m_Slot->frame_reserved_bytes.Load(EMemoryOrder::Acquire);
        for (;;) {
            if (RequestedBytes > m_Slot->hard_budget_bytes || Current > m_Slot->hard_budget_bytes - RequestedBytes) {
                return false;
            }
            if (m_Slot->frame_reserved_bytes.CompareExchange(Current, Current + RequestedBytes)) {
                return true;
            }
        }
    }

    /** 具象arenaの確保失敗時だけ、先に確保した予算予約を戻す。 */
    void RollBackFrameBudget(usize Size) noexcept
    {
        if (m_Slot->use_frame_allocator && m_Slot->hard_budget_bytes != 0u) {
            m_Slot->frame_reserved_bytes.FetchSub(static_cast<u64>(Size));
        }
    }

    /**
     * 現在のスロットの裏側アロケータ (arena か mimalloc) を返す。
     *
     * @return use_frame_allocator に応じた裏側アロケータ。
     */
    FAllocator* Backing() const noexcept
    {
        return m_Slot->use_frame_allocator ? static_cast<FAllocator*>(m_Slot->arena)
                                           : static_cast<FAllocator*>(&m_Slot->mimalloc);
    }

    /** 委譲先のセグメントスロット (未バインドなら nullptr)。 */
    SegmentSlot* m_Slot = nullptr;

    /** 現在の MemorySystem 初期化寿命。0 は無効状態。 */
    TAtomic<u64> m_LifetimeGeneration{0};
};

/** プロセスに 1 つだけのグローバル状態。 */
struct State {
    /** セグメント種別ごとのスロット。 */
    SegmentSlot slots[(usize)ESegment::_Count];

    /** セグメント種別ごとの公開アダプタ。 */
    SegmentAllocator allocators[(usize)ESegment::_Count];

    /** FMemorySystem が初期化済みか。 */
    bool inited = false;

    /** install_as_default_allocator で既定アロケータを差し替えたか。 */
    bool default_overridden = false;

    /** 差し替え前の既定アロケータ (Shutdown で復元する退避先)。 */
    FAllocator* prev_default = nullptr;
};

/** プロセス唯一のグローバル状態インスタンス。 */
State g_state;

/** 公開 enum 値を配列添字へ変換できるか確認する。 */
bool IsValidSegment(ESegment Segment) noexcept
{
    return static_cast<usize>(Segment) < static_cast<usize>(ESegment::_Count);
}

/** スロットが選択している具象アロケータを返す。 */
FAllocator* BackingAllocator(SegmentSlot& Slot) noexcept
{
    return Slot.use_frame_allocator ? static_cast<FAllocator*>(Slot.arena) : static_cast<FAllocator*>(&Slot.mimalloc);
}

/** 初期化済みスロットを破棄し、再初期化可能な状態へ戻す。 */
void DestroySegmentSlot(SegmentSlot& Slot) noexcept
{
    if (!Slot.initialized) {
        return;
    }

    if (Slot.use_frame_allocator) {
        if (Slot.arena) {
            Slot.arena->~FArenaAllocator();
            (void)::HeapFree(::GetProcessHeap(), 0, Slot.arena);
            Slot.arena = nullptr;
        }
    } else {
        Slot.mimalloc.Shutdown();
    }
    Slot.initialized = false;
    Slot.hard_budget_bytes = 0;
    Slot.frame_reserved_bytes.Store(0u, EMemoryOrder::Release);
    Slot.frame_active_operations.Store(0u, EMemoryOrder::Release);
    Slot.frame_reset_in_progress.Store(0u, EMemoryOrder::Release);
}

/** スロットの通常統計と、利用可能ならバックエンド独立走査をまとめる。 */
MemorySegmentInspection InspectSegmentSlot(SegmentSlot& Slot) noexcept
{
    MemorySegmentInspection Result;
    Result.segment = Slot.segment;
    if (!Slot.initialized) {
        return Result;
    }

    FAllocator* const Allocator = BackingAllocator(Slot);
    Result.allocator_name = Allocator->Name();
    Result.requested_bytes = Allocator->BytesAllocated();
    Result.outstanding_allocation_count = Allocator->AllocationCount();
    if (Slot.use_frame_allocator) {
        return Result;
    }

    const MimallocHeapInspectionStatistics Inspection = Slot.mimalloc.InspectHeap();
    Result.reserved_bytes = Inspection.reserved_bytes;
    Result.committed_bytes = Inspection.committed_bytes;
    Result.usable_bytes = Inspection.usable_bytes;
    Result.requested_bytes = Inspection.requested_bytes;
    Result.outstanding_allocation_count = Inspection.allocation_count;
    Result.backend_inspection_supported = true;
    Result.visit_succeeded = Inspection.visit_succeeded;
    Result.metadata_valid = Inspection.metadata_valid;
    Result.matches_authoritative_statistics = Inspection.matches_authoritative_statistics;
    return Result;
}

} // namespace

// 既定設定（小規模テスト用、すぐ動かしたいとき向け）
MemorySystemConfig FMemorySystem::DefaultConfig() noexcept
{
    MemorySystemConfig Configuration{};
    auto Setup = [](SegmentConfig& SegmentConfiguration, ESegment Segment, u64 HardBudgetBytes,
                    bool bUseFrameAllocator) {
        SegmentConfiguration.segment = Segment;
        SegmentConfiguration.hard_budget_bytes = HardBudgetBytes;
        SegmentConfiguration.use_frame_allocator = bUseFrameAllocator;
    };
    Setup(Configuration.segments[(usize)ESegment::Default], ESegment::Default, 256ull << 20, false);
    Setup(Configuration.segments[(usize)ESegment::Permanent], ESegment::Permanent, 64ull << 20, false);
    Setup(Configuration.segments[(usize)ESegment::Temp], ESegment::Temp, 32ull << 20, true);
    Setup(Configuration.segments[(usize)ESegment::Resource], ESegment::Resource, 512ull << 20, false);
    Setup(Configuration.segments[(usize)ESegment::Develop], ESegment::Develop, 64ull << 20, false);
    return Configuration;
}

// 全セグメントを設定で初期化
TResult<void> FMemorySystem::Init(const MemorySystemConfig& Configuration) noexcept
{
    if (g_state.inited) {
        return ACS_ERR(Memory, 30, "FMemorySystem already initialized");
    }

    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        if (Configuration.segments[Index].segment != static_cast<ESegment>(Index)) {
            return ACS_ERR(Memory, 32, "MemorySystem segment configuration order is invalid");
        }
    }

    BeginAllocationTrackingLifecycle();

    // 途中で失敗した場合、初期化済み first-class heap / arena をすべてロールバックする。
    usize InitializedCount = 0;
    TResult<void> Failure = Ok();

    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        const SegmentConfig& SegmentConfiguration = Configuration.segments[Index];
        SegmentSlot& Slot = g_state.slots[Index];
        Slot.segment = SegmentConfiguration.segment;
        Slot.use_frame_allocator = SegmentConfiguration.use_frame_allocator;
        Slot.hard_budget_bytes = SegmentConfiguration.hard_budget_bytes;
        Slot.frame_reserved_bytes.Store(0u, EMemoryOrder::Release);
        Slot.frame_active_operations.Store(0u, EMemoryOrder::Release);
        Slot.frame_reset_in_progress.Store(0u, EMemoryOrder::Release);

        if (Slot.use_frame_allocator) {
            // ページは既定差し替えの影響を受けない system allocator から確保する。
            Slot.arena = static_cast<FArenaAllocator*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(FArenaAllocator)));
            if (!Slot.arena) {
                Failure = ACS_ERR(Memory, 31, "frame arena allocation failed");
                break;
            }
            ::new (Slot.arena) FArenaAllocator(1u * 1024u * 1024u, &g_frame_backing);
        } else {
            Failure = Slot.mimalloc.Init(SegmentConfiguration.hard_budget_bytes);
            if (Failure.IsErr()) {
                break;
            }
        }
        Slot.initialized = true;
        g_state.allocators[Index].Bind(&Slot);
        InitializedCount = Index + 1;
    }

    if (Failure.IsErr()) {
        for (usize Index = 0; Index < InitializedCount; ++Index) {
            g_state.allocators[Index].InvalidateLifetime();
            DestroySegmentSlot(g_state.slots[Index]);
        }
        EndAllocationTrackingLifecycle();
        return Failure;
    }

    g_state.inited = true;
    if (Configuration.install_as_default_allocator) {
        g_state.prev_default = &DefaultAllocator();
        SetDefaultAllocator(&g_state.allocators[(usize)ESegment::Default]);
        g_state.default_overridden = true;
    }

#if ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE
    constexpr const char* kReEnginePageProfileEnabled = "true";
#else
    constexpr const char* kReEnginePageProfileEnabled = "false";
#endif
    ACS_LOG_INFO("[acs][memory] initialized=true general_allocator=mimalloc mimalloc_version=%d "
                 "re_engine_page_profile=%s frame_allocator=arena",
                 FMimallocAllocator::RuntimeVersion(), kReEnginePageProfileEnabled);
    return Ok();
}

void FMemorySystem::Shutdown() noexcept
{
    if (!g_state.inited) {
        return;
    }

    // backing を破棄する前に全公開アダプタ寿命を無効化し、遅延解放を旧ヒープへ到達させない。
    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        g_state.allocators[Index].InvalidateLifetime();
    }

    // frame arena は一括寿命なので、終了診断の対象外として先に巻き戻す。
    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        SegmentSlot& Slot = g_state.slots[Index];
        if (Slot.initialized && Slot.use_frame_allocator) {
            if (Slot.arena) {
                Slot.arena->Reset(false);
            }
            Slot.frame_reserved_bytes.Store(0u, EMemoryOrder::Release);
            ForgetTrackedSegment(Slot.segment);
        }
    }

    // 診断ログ自身が Default セグメントへ新規確保されないよう、集計より前に復元する。
    if (g_state.default_overridden) {
        SetDefaultAllocator(g_state.prev_default);
        g_state.default_overridden = false;
        g_state.prev_default = nullptr;
    }

    const MemoryLeakSummary LeakSummary = CaptureLeakSummary();
    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        SegmentSlot& Slot = g_state.slots[Index];
        if (!Slot.initialized || Slot.use_frame_allocator) {
            continue;
        }

        Slot.mimalloc.Collect(true);
        const MemorySegmentInspection Inspection = InspectSegmentSlot(Slot);
        const bool bInspectionConsistent = Inspection.visit_succeeded && Inspection.metadata_valid &&
                                           Inspection.matches_authoritative_statistics;
        if (bInspectionConsistent) {
            ACS_LOG_INFO("[acs][memory] tracker=mimalloc_heap segment=%s inspection_succeeded=true "
                         "metadata_valid=true counters_match=true reserved_bytes=%llu committed_bytes=%llu "
                         "usable_bytes=%llu outstanding_allocations=%llu outstanding_bytes=%llu",
                         ToString(Slot.segment), static_cast<unsigned long long>(Inspection.reserved_bytes),
                         static_cast<unsigned long long>(Inspection.committed_bytes),
                         static_cast<unsigned long long>(Inspection.usable_bytes),
                         static_cast<unsigned long long>(Inspection.outstanding_allocation_count),
                         static_cast<unsigned long long>(Inspection.requested_bytes));
        } else {
            ACS_LOG_ERROR("[acs][memory] tracker=mimalloc_heap segment=%s inspection_succeeded=%s "
                          "metadata_valid=%s counters_match=%s reserved_bytes=%llu committed_bytes=%llu "
                          "usable_bytes=%llu outstanding_allocations=%llu outstanding_bytes=%llu",
                          ToString(Slot.segment), Inspection.visit_succeeded ? "true" : "false",
                          Inspection.metadata_valid ? "true" : "false",
                          Inspection.matches_authoritative_statistics ? "true" : "false",
                          static_cast<unsigned long long>(Inspection.reserved_bytes),
                          static_cast<unsigned long long>(Inspection.committed_bytes),
                          static_cast<unsigned long long>(Inspection.usable_bytes),
                          static_cast<unsigned long long>(Inspection.outstanding_allocation_count),
                          static_cast<unsigned long long>(Inspection.requested_bytes));
        }

        const u64 OutstandingBytes = Slot.mimalloc.BytesAllocated();
        const u64 AllocationCount = Slot.mimalloc.AllocationCount();
        if (OutstandingBytes != 0 || AllocationCount != 0) {
            ACS_LOG_ERROR("[acs][memory] segment=%s allocator=mimalloc leak_detected=true "
                          "outstanding_allocations=%llu outstanding_bytes=%llu peak_bytes=%llu",
                          ToString(Slot.segment), static_cast<unsigned long long>(AllocationCount),
                          static_cast<unsigned long long>(OutstandingBytes),
                          static_cast<unsigned long long>(Slot.mimalloc.PeakBytes()));
        }
    }

    if (LeakSummary.HasLeaks() && IsAllocationSiteTrackingEnabled()) {
        const MemoryTrackingReport TrackingReport = DumpOutstandingMemoryAllocations();
        if (!TrackingReport.IsComplete() ||
            TrackingReport.outstanding_allocation_count != LeakSummary.outstanding_allocation_count) {
            ACS_LOG_ERROR("[acs][memory] allocation_tracking_consistent=false allocator_count=%llu "
                          "tracked_count=%llu tracking_complete=%s dropped_tracking_events=%llu",
                          static_cast<unsigned long long>(LeakSummary.outstanding_allocation_count),
                          static_cast<unsigned long long>(TrackingReport.outstanding_allocation_count),
                          TrackingReport.IsComplete() ? "true" : "false",
                          static_cast<unsigned long long>(TrackingReport.dropped_tracking_event_count));
        }
    }
    if (!LeakSummary.HasLeaks()) {
        ACS_LOG_INFO("[acs][memory] leak_detected=false outstanding_allocations=0 "
                     "outstanding_bytes=0 leaking_segments=0");
    } else {
        ACS_LOG_ERROR("[acs][memory] leak_detected=true outstanding_allocations=%llu "
                      "outstanding_bytes=%llu leaking_segments=%u",
                      static_cast<unsigned long long>(LeakSummary.outstanding_allocation_count),
                      static_cast<unsigned long long>(LeakSummary.outstanding_bytes),
                      LeakSummary.leaking_segment_count);
    }

    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        DestroySegmentSlot(g_state.slots[Index]);
    }
    g_state.inited = false;
    EndAllocationTrackingLifecycle();
}

FAllocator* FMemorySystem::Get(ESegment Segment) noexcept
{
    if (!g_state.inited || !IsValidSegment(Segment)) {
        return nullptr;
    }
    return &g_state.allocators[static_cast<usize>(Segment)];
}

ESegment FMemorySystem::Current() noexcept
{
    return tls_current_segment;
}

FAllocator* FMemorySystem::CurrentAllocator() noexcept
{
    return Get(tls_current_segment);
}

// Temp セグメントを巻き戻す（フレーム先頭で 1 回呼ぶ）
void FMemorySystem::ResetTemp() noexcept
{
    if (!g_state.inited) {
        return;
    }
    SegmentSlot& Slot = g_state.slots[(usize)ESegment::Temp];
    if (Slot.initialized && Slot.use_frame_allocator && Slot.arena) {
        BeginFrameReset(Slot);
        Slot.arena->Reset(false); // ページは保持して次フレームで再利用 (再確保なし)
        Slot.frame_reserved_bytes.Store(0u, EMemoryOrder::Release);
        ForgetTrackedSegment(Slot.segment);
        EndFrameReset(Slot);
    }
}

// 全セグメントの統計を Output に詰める
u32 FMemorySystem::GetStats(SegmentStats* Output, u32 OutputCapacity) noexcept
{
    if (!g_state.inited || !Output || OutputCapacity == 0) {
        return 0;
    }
    u32 WrittenCount = 0;
    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count) && WrittenCount < OutputCapacity; ++Index) {
        SegmentSlot& Slot = g_state.slots[Index];
        if (!Slot.initialized) {
            continue;
        }
        FAllocator* const Allocator = BackingAllocator(Slot);
        SegmentStats& Statistics = Output[WrittenCount++];
        Statistics.segment = Slot.segment;
        Statistics.segment_name = ToString(Slot.segment);
        Statistics.allocator_name = Allocator->Name();
        Statistics.requested_bytes = Allocator->BytesAllocated();
        Statistics.peak_requested_bytes = Allocator->PeakBytes();
        Statistics.hard_budget_bytes = Slot.hard_budget_bytes;
        Statistics.outstanding_allocation_count = Allocator->AllocationCount();
    }
    return WrittenCount;
}

MemorySegmentInspection FMemorySystem::InspectSegmentMemory(ESegment Segment) noexcept
{
    MemorySegmentInspection Result;
    Result.segment = Segment;
    if (!g_state.inited || !IsValidSegment(Segment)) {
        return Result;
    }
    return InspectSegmentSlot(g_state.slots[static_cast<usize>(Segment)]);
}

MemoryLeakSummary FMemorySystem::CaptureLeakSummary() noexcept
{
    MemoryLeakSummary Summary;
    if (!g_state.inited) {
        return Summary;
    }

    for (usize Index = 0; Index < static_cast<usize>(ESegment::_Count); ++Index) {
        SegmentSlot& Slot = g_state.slots[Index];
        if (!Slot.initialized || Slot.use_frame_allocator) {
            continue;
        }
        const u64 OutstandingBytes = Slot.mimalloc.BytesAllocated();
        const u64 AllocationCount = Slot.mimalloc.AllocationCount();
        Summary.outstanding_bytes += OutstandingBytes;
        Summary.outstanding_allocation_count += AllocationCount;
        if (OutstandingBytes != 0 || AllocationCount != 0) {
            ++Summary.leaking_segment_count;
        }
    }
    return Summary;
}

MemoryTrackingCheckpoint FMemorySystem::CaptureMemoryTrackingCheckpoint() noexcept
{
    MemoryTrackingCheckpoint Checkpoint;
#if ACS_MEMORY_TRACK_ALLOCATION_SITES
    ::AcquireSRWLockShared(&g_allocation_tracking.lock);
    if (g_allocation_tracking.active) {
        Checkpoint.allocation_sequence = g_allocation_tracking.newest_allocation_sequence;
    }
    ::ReleaseSRWLockShared(&g_allocation_tracking.lock);
#endif
    return Checkpoint;
}

MemoryTrackingReport FMemorySystem::CollectOutstandingMemoryAllocations(MemoryTrackingCheckpoint Checkpoint,
                                                                        OutstandingMemoryAllocation* Output,
                                                                        u32 OutputCapacity) noexcept
{
    return CollectTrackedAllocations(Checkpoint, Output, OutputCapacity);
}

MemoryTrackingReport FMemorySystem::DumpOutstandingMemoryAllocations(MemoryTrackingCheckpoint Checkpoint,
                                                                     u32 MaximumLoggedAllocationCount) noexcept
{
    MemoryTrackingReport Report = CollectTrackedAllocations(Checkpoint, nullptr, 0);
    const bool bPreviousIgnoreState = tls_ignore_allocation_tracking;
    tls_ignore_allocation_tracking = true;

    if (!Report.tracking_enabled) {
        ACS_LOG_INFO("[acs][memory] allocation_site_tracking=disabled");
        tls_ignore_allocation_tracking = bPreviousIgnoreState;
        return Report;
    }

    u32 DetailCapacity = MaximumLoggedAllocationCount;
    if (Report.outstanding_allocation_count < DetailCapacity) {
        DetailCapacity = static_cast<u32>(Report.outstanding_allocation_count);
    }
    OutstandingMemoryAllocation* Details = nullptr;
    if (DetailCapacity > 0) {
        Details = static_cast<OutstandingMemoryAllocation*>(
            ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY,
                        static_cast<usize>(DetailCapacity) * sizeof(OutstandingMemoryAllocation)));
    }
    if (Details) {
        Report = CollectTrackedAllocations(Checkpoint, Details, DetailCapacity);
        for (u64 i = 1; i < Report.written_allocation_count; ++i) {
            const OutstandingMemoryAllocation Value = Details[i];
            u64 Position = i;
            while (Position > 0 && Details[Position - 1].allocation_sequence > Value.allocation_sequence) {
                Details[Position] = Details[Position - 1];
                --Position;
            }
            Details[Position] = Value;
        }
    }

    if (Report.outstanding_allocation_count == 0) {
        ACS_LOG_INFO("[acs][memory] allocation_site_leak_detected=false outstanding_allocations=0 "
                     "outstanding_requested_bytes=0 tracking_complete=%s dropped_tracking_events=%llu",
                     Report.IsComplete() ? "true" : "false",
                     static_cast<unsigned long long>(Report.dropped_tracking_event_count));
    } else {
        ACS_LOG_ERROR("[acs][memory] allocation_site_leak_detected=true outstanding_allocations=%llu "
                      "outstanding_requested_bytes=%llu tracking_complete=%s dropped_tracking_events=%llu "
                      "checkpoint_sequence=%llu newest_allocation_sequence=%llu",
                      static_cast<unsigned long long>(Report.outstanding_allocation_count),
                      static_cast<unsigned long long>(Report.outstanding_requested_bytes),
                      Report.IsComplete() ? "true" : "false",
                      static_cast<unsigned long long>(Report.dropped_tracking_event_count),
                      static_cast<unsigned long long>(Checkpoint.allocation_sequence),
                      static_cast<unsigned long long>(Report.newest_allocation_sequence));

        if (DetailCapacity > 0 && !Details) {
            ACS_LOG_ERROR("[acs][memory] allocation_site_details=unavailable reason=process_heap_allocation_failed");
        }
        for (u64 i = 0; Details && i < Report.written_allocation_count; ++i) {
            const OutstandingMemoryAllocation& Allocation = Details[i];
            ACS_LOG_ERROR("[acs][memory] allocation_sequence=%llu address=%p requested_bytes=%llu "
                          "alignment=%llu segment=%s file=\"%s\" line=%u column=%u function=\"%s\"",
                          static_cast<unsigned long long>(Allocation.allocation_sequence), Allocation.address,
                          static_cast<unsigned long long>(Allocation.requested_bytes),
                          static_cast<unsigned long long>(Allocation.alignment), ToString(Allocation.segment),
                          Allocation.source_file ? Allocation.source_file : "", Allocation.source_line,
                          Allocation.source_column, Allocation.source_function ? Allocation.source_function : "");
        }
        if (Report.outstanding_allocation_count > Report.written_allocation_count) {
            ACS_LOG_ERROR("[acs][memory] allocation_site_details_omitted=%llu maximum_logged_allocations=%u",
                          static_cast<unsigned long long>(Report.outstanding_allocation_count -
                                                          Report.written_allocation_count),
                          MaximumLoggedAllocationCount);
        }
    }

    if (Details) ::HeapFree(::GetProcessHeap(), 0, Details);
    tls_ignore_allocation_tracking = bPreviousIgnoreState;
    return Report;
}

void FMemorySystem::SetBreakOnAllocationSequence(u64 AllocationSequence) noexcept
{
#if ACS_MEMORY_TRACK_ALLOCATION_SITES
    ::AcquireSRWLockExclusive(&g_allocation_tracking.lock);
    g_allocation_tracking.break_on_allocation_sequence = AllocationSequence;
    ::ReleaseSRWLockExclusive(&g_allocation_tracking.lock);
#else
    (void)AllocationSequence;
#endif
}

bool FMemorySystem::IsAllocationSiteTrackingEnabled() noexcept
{
#if ACS_MEMORY_TRACK_ALLOCATION_SITES
    return true;
#else
    return false;
#endif
}

// コンストラクタで TLS の現在セグメントを上書き、デストラクタで元に戻す
ScopedMemorySegment::ScopedMemorySegment(ESegment Segment) noexcept : m_Previous(tls_current_segment)
{
    if (IsValidSegment(Segment)) {
        tls_current_segment = Segment;
    }
}

ScopedMemorySegment::~ScopedMemorySegment() noexcept
{
    tls_current_segment = m_Previous;
}

} // namespace acs
