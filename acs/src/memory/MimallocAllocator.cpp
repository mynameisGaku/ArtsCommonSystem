// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - mimalloc first-class heap アロケータ実装
// =============================================================================
#include "memory/MimallocAllocator.h"

#include "foundation/Platform.h"
#include "memory/Memory.h"

#include <mimalloc.h>

namespace acs {

namespace {

/** 全 FMimallocAllocator に共通する mimalloc プロセス設定の初期化状態。 */
TAtomic<u32> g_process_configuration_state{0};

/** 全アロケータの Init 世代を一意化する採番器。0 は未初期化用に予約する。 */
TAtomic<u64> g_generation_sequence{0};

/** ACS 管理ブロックの正当性確認値。 */
constexpr u64 kAllocationHeaderMagic = 0x4143534D494D4133ull;

/** 利用者ポインタ直前に置く復元情報の正当性確認値。 */
constexpr u64 kAllocationPrefixMagic = 0x5052454649584D49ull;

/** 低アライメント要求でも復元情報を自然整列で読み書きするための下限。 */
constexpr usize kMinimumEffectiveAlignment = alignof(void*);

/** Capcom の早期返却方針を modern mimalloc の公開 option へ写した purge 遅延。 */
constexpr long kPurgeDelayMilliseconds = 25;

/** 汎用確保の一定回数ごとに段階的収集を進める間隔。 */
constexpr long kGenericCollectionAllocationInterval = 4096;

/** 最初の arena 予約を 64 MiB に抑え、巨大な仮想予約を既定にしない。値の単位は KiB。 */
constexpr long kArenaReserveKibibytes = 64 * 1024;

/**
 * mimalloc の生ブロック先頭に置く追跡ヘッダ。
 *
 * @details ブロック列挙時はこのヘッダだけで要求量と所有者を復元できる。
 */
struct AllocationHeader {
    u64 Magic = 0;
    const FMimallocAllocator* Owner = nullptr;
    u64 Generation = 0;
    u64 RequestedBytes = 0;
    usize EffectiveAlignment = 0;
    usize UserOffset = 0;
};

/** 利用者ポインタから mimalloc 生ブロックへ戻るための固定長プレフィックス。 */
struct AllocationPrefix {
    void* AllocationBase = nullptr;
    u64 Magic = 0;
};

static_assert((sizeof(AllocationPrefix) % alignof(void*)) == 0, "AllocationPrefix must preserve pointer alignment");

/** 固定長かつ動的確保なしで破棄時診断を組み立てる。 */
class FMimallocDiagnosticBuffer final {
public:
    /** 文字列を末尾へ追加する。容量超過時は切り詰める。 */
    void AppendText(const char* Text) noexcept
    {
        if (!Text) {
            return;
        }
        while (*Text != '\0' && m_Length + 1u < sizeof(m_Data)) {
            m_Data[m_Length++] = *Text++;
        }
        m_Data[m_Length] = '\0';
    }

    /** 符号なし整数を 10 進表記で追加する。 */
    void AppendUnsigned(u64 Value) noexcept
    {
        char Reversed[32];
        usize Count = 0;
        do {
            Reversed[Count++] = static_cast<char>('0' + (Value % 10u));
            Value /= 10u;
        } while (Value != 0 && Count < sizeof(Reversed));

        while (Count > 0) {
            const char Digit[2] = {Reversed[--Count], '\0'};
            AppendText(Digit);
        }
    }

    /** ポインタ値を 16 進表記で追加する。 */
    void AppendPointer(const void* Pointer) noexcept
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        AppendText("0x");
        uptr Value = reinterpret_cast<uptr>(Pointer);
        char Reversed[sizeof(uptr) * 2u];
        usize Count = 0;
        do {
            Reversed[Count++] = kHexDigits[Value & 0xFu];
            Value >>= 4u;
        } while (Value != 0 && Count < sizeof(Reversed));

        while (Count > 0) {
            const char Digit[2] = {Reversed[--Count], '\0'};
            AppendText(Digit);
        }
    }

    /** 診断文字列の先頭を返す。 */
    const char* Data() const noexcept
    {
        return m_Data;
    }

    /** 終端 null を除く文字列長を返す。 */
    u32 Length() const noexcept
    {
        return static_cast<u32>(m_Length);
    }

private:
    char m_Data[512] = {};
    usize m_Length = 0;
};

/** 加算を飽和させ、診断統計のラップを防ぐ。 */
u64 SaturatingAdd(u64 Left, u64 Right) noexcept
{
    const u64 Maximum = ~u64(0);
    return Right > Maximum - Left ? Maximum : Left + Right;
}

/** mimalloc のプロセス option を競合なく一度だけ設定する。 */
void ConfigureMimallocProcessOnce() noexcept
{
    u32 State = g_process_configuration_state.Load(EMemoryOrder::Acquire);
    if (State == 2u) {
        return;
    }

    u32 Expected = 0u;
    if (g_process_configuration_state.CompareExchange(Expected, 1u)) {
        mi_option_set_enabled(mi_option_show_errors, true);
        mi_option_set_enabled(mi_option_deprecated_eager_commit, false);
        mi_option_set_enabled(mi_option_arena_eager_commit, false);
        mi_option_set_enabled(mi_option_page_commit_on_demand, true);
        mi_option_set_enabled(mi_option_purge_decommits, true);
        mi_option_set(mi_option_purge_delay, kPurgeDelayMilliseconds);
        mi_option_set(mi_option_arena_purge_mult, 1);
        mi_option_set(mi_option_arena_reserve, kArenaReserveKibibytes);
        mi_option_set(mi_option_generic_collect, kGenericCollectionAllocationInterval);
        mi_option_set(mi_option_page_reclaim_on_free, 1);
        mi_option_set(mi_option_page_full_retain, 0);
        mi_option_set(mi_option_page_cross_thread_max_reclaim, 8);
        mi_option_set_enabled(mi_option_visit_abandoned, true);
        mi_option_set(mi_option_retry_on_oom, 0);
        g_process_configuration_state.Store(2u, EMemoryOrder::Release);
        return;
    }

    while (g_process_configuration_state.Load(EMemoryOrder::Acquire) != 2u) {
#if ACS_PLATFORM_WINDOWS
        YieldProcessor();
#endif
    }
}

/** Init ごとに 0 以外の世代番号を採番する。 */
u64 AllocateGeneration() noexcept
{
    u64 Generation = g_generation_sequence.FetchAdd(1u) + 1u;
    if (Generation == 0u) {
        Generation = g_generation_sequence.FetchAdd(1u) + 1u;
    }
    return Generation;
}

/** 要求値を mimalloc 生ブロックに必要な総サイズへ安全に変換する。 */
bool CalculateAllocationSize(usize RequestedSize, usize EffectiveAlignment, usize& AllocationSize) noexcept
{
    const usize Maximum = ~usize(0);
    const usize FixedOverhead = sizeof(AllocationHeader) + sizeof(AllocationPrefix);
    if (EffectiveAlignment == 0 || FixedOverhead > Maximum - (EffectiveAlignment - 1u)) {
        AllocationSize = 0;
        return false;
    }

    const usize Overhead = FixedOverhead + EffectiveAlignment - 1u;
    if (RequestedSize > Maximum - Overhead) {
        AllocationSize = 0;
        return false;
    }

    AllocationSize = RequestedSize + Overhead;
    return true;
}

/** 生ブロックに ACS ヘッダと利用者ポインタ直前プレフィックスを設定する。 */
void* InitializeAllocationMetadata(void* AllocationBase, FMimallocAllocator* Owner, u64 Generation, usize RequestedSize,
                                   usize EffectiveAlignment) noexcept
{
    auto* Header = static_cast<AllocationHeader*>(AllocationBase);
    const uptr FirstUserAddress = reinterpret_cast<uptr>(AllocationBase) + sizeof(AllocationHeader) +
                                  sizeof(AllocationPrefix);
    const uptr UserAddress = AlignUp(FirstUserAddress, EffectiveAlignment);
    auto* Prefix = reinterpret_cast<AllocationPrefix*>(UserAddress - sizeof(AllocationPrefix));

    Header->Magic = kAllocationHeaderMagic;
    Header->Owner = Owner;
    Header->Generation = Generation;
    Header->RequestedBytes = static_cast<u64>(RequestedSize);
    Header->EffectiveAlignment = EffectiveAlignment;
    Header->UserOffset = static_cast<usize>(UserAddress - reinterpret_cast<uptr>(AllocationBase));

    Prefix->AllocationBase = AllocationBase;
    Prefix->Magic = kAllocationPrefixMagic;
    return reinterpret_cast<void*>(UserAddress);
}

/** 利用者ポインタからプレフィックスを得る。呼び出し前にヒープ領域内確認が必要。 */
const AllocationPrefix* PrefixFromUserPointer(const void* Pointer) noexcept
{
    return reinterpret_cast<const AllocationPrefix*>(static_cast<const u8*>(Pointer) - sizeof(AllocationPrefix));
}

/** 所有者、世代、位置、アライメントを含めてヘッダとプレフィックスを検証する。 */
const AllocationHeader* ValidateAllocationMetadata(const FMimallocAllocator* Owner, const void* Heap, u64 Generation,
                                                   const void* Pointer) noexcept
{
    if (!Owner || !Heap || !Pointer) {
        return nullptr;
    }

    const auto* MimallocHeap = static_cast<const mi_heap_t*>(Heap);
    if (!mi_heap_contains(MimallocHeap, Pointer)) {
        return nullptr;
    }

    const uptr PointerAddress = reinterpret_cast<uptr>(Pointer);
    if ((PointerAddress & (alignof(void*) - 1u)) != 0u || PointerAddress < sizeof(AllocationPrefix)) {
        return nullptr;
    }

    const uptr PrefixAddress = PointerAddress - sizeof(AllocationPrefix);
    const auto* Prefix = reinterpret_cast<const AllocationPrefix*>(PrefixAddress);
    if (!mi_heap_contains(MimallocHeap, Prefix)) {
        return nullptr;
    }
    if (Prefix->Magic != kAllocationPrefixMagic || !Prefix->AllocationBase) {
        return nullptr;
    }
    if ((reinterpret_cast<uptr>(Prefix->AllocationBase) & (alignof(AllocationHeader) - 1u)) != 0u) {
        return nullptr;
    }
    if (!mi_heap_contains(MimallocHeap, Prefix->AllocationBase)) {
        return nullptr;
    }

    const auto* Header = static_cast<const AllocationHeader*>(Prefix->AllocationBase);
    if (Header->Magic != kAllocationHeaderMagic || Header->Owner != Owner || Header->Generation != Generation ||
        Header->RequestedBytes == 0u || !IsPow2(Header->EffectiveAlignment) ||
        Header->EffectiveAlignment < kMinimumEffectiveAlignment) {
        return nullptr;
    }

    const usize UsableSize = mi_usable_size(Prefix->AllocationBase);
    const usize MinimumUserOffset = sizeof(AllocationHeader) + sizeof(AllocationPrefix);
    if (Header->UserOffset < MinimumUserOffset || Header->UserOffset > UsableSize ||
        Header->RequestedBytes > static_cast<u64>(UsableSize - Header->UserOffset)) {
        return nullptr;
    }

    const uptr BaseAddress = reinterpret_cast<uptr>(Prefix->AllocationBase);
    if (Header->UserOffset > (~uptr(0)) - BaseAddress) {
        return nullptr;
    }
    const uptr ExpectedAddress = BaseAddress + Header->UserOffset;
    if (ExpectedAddress != reinterpret_cast<uptr>(Pointer) ||
        (reinterpret_cast<uptr>(Pointer) & (Header->EffectiveAlignment - 1u)) != 0u) {
        return nullptr;
    }
    return Header;
}

/** 未解放を伴う破棄を Logger に依存せず stderr とデバッガへ出力する。 */
void EmitDestroyedWithLiveAllocations(const FMimallocAllocator* Allocator, u64 OutstandingAllocationCount,
                                      u64 OutstandingBytes, u64 HardBudgetBytes) noexcept
{
    FMimallocDiagnosticBuffer Message;
    Message.AppendText("[acs][memory] allocator=Mimalloc allocator_address=");
    Message.AppendPointer(Allocator);
    Message.AppendText(" destroyed_with_live_allocations=true outstanding_allocations=");
    Message.AppendUnsigned(OutstandingAllocationCount);
    Message.AppendText(" outstanding_bytes=");
    Message.AppendUnsigned(OutstandingBytes);
    Message.AppendText(" hard_budget_bytes=");
    Message.AppendUnsigned(HardBudgetBytes);
    Message.AppendText("\r\n");

#if ACS_PLATFORM_WINDOWS
    ::OutputDebugStringA(Message.Data());
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (StandardError && StandardError != INVALID_HANDLE_VALUE) {
        DWORD Written = 0;
        (void)::WriteFile(StandardError, Message.Data(), Message.Length(), &Written, nullptr);
    }
#endif
}

/** ブロック列挙コールバックが更新する内部状態。 */
struct HeapInspectionContext {
    const FMimallocAllocator* Owner = nullptr;
    u64 Generation = 0;
    MimallocHeapInspectionStatistics Statistics = {};
};

/** mi_heap_visit_blocks の領域通知と生存ブロック通知を集計する。 */
bool mi_cdecl VisitHeapBlock(const mi_heap_t*, const mi_heap_area_t* Area, void* Block, size_t BlockSize,
                             void* Argument)
{
    auto* Context = static_cast<HeapInspectionContext*>(Argument);
    if (!Context || !Area) {
        return false;
    }

    if (!Block) {
        ++Context->Statistics.area_count;
        Context->Statistics.reserved_bytes = SaturatingAdd(Context->Statistics.reserved_bytes,
                                                           static_cast<u64>(Area->reserved));
        Context->Statistics.committed_bytes = SaturatingAdd(Context->Statistics.committed_bytes,
                                                            static_cast<u64>(Area->committed));
        return true;
    }

    ++Context->Statistics.allocation_count;
    Context->Statistics.usable_bytes = SaturatingAdd(Context->Statistics.usable_bytes, static_cast<u64>(BlockSize));

    if (BlockSize < sizeof(AllocationHeader)) {
        Context->Statistics.metadata_valid = false;
        return true;
    }

    const auto* Header = static_cast<const AllocationHeader*>(Block);
    const usize MinimumUserOffset = sizeof(AllocationHeader) + sizeof(AllocationPrefix);
    if (Header->Magic != kAllocationHeaderMagic || Header->Owner != Context->Owner ||
        Header->Generation != Context->Generation || Header->RequestedBytes == 0u ||
        !IsPow2(Header->EffectiveAlignment) || Header->EffectiveAlignment < kMinimumEffectiveAlignment ||
        Header->UserOffset < MinimumUserOffset || Header->UserOffset > BlockSize ||
        Header->RequestedBytes > static_cast<u64>(BlockSize - Header->UserOffset)) {
        Context->Statistics.metadata_valid = false;
        return true;
    }

    const uptr BlockAddress = reinterpret_cast<uptr>(Block);
    if (Header->UserOffset > (~uptr(0)) - BlockAddress) {
        Context->Statistics.metadata_valid = false;
        return true;
    }
    const uptr UserAddress = BlockAddress + Header->UserOffset;
    const auto* Prefix = reinterpret_cast<const AllocationPrefix*>(UserAddress - sizeof(AllocationPrefix));
    if ((UserAddress & (Header->EffectiveAlignment - 1u)) != 0u || Prefix->Magic != kAllocationPrefixMagic ||
        Prefix->AllocationBase != Block) {
        Context->Statistics.metadata_valid = false;
        return true;
    }

    Context->Statistics.requested_bytes = SaturatingAdd(Context->Statistics.requested_bytes, Header->RequestedBytes);
    return true;
}

} // namespace

FMimallocAllocator::~FMimallocAllocator() noexcept
{
    Shutdown();
}

TResult<void> FMimallocAllocator::Init(u64 BudgetBytes) noexcept
{
    if (m_Heap) {
        return ACS_ERR(Memory, 70, "FMimallocAllocator already initialized");
    }

    ConfigureMimallocProcessOnce();
    mi_heap_t* Heap = mi_heap_new();
    if (!Heap) {
        return ACS_ERR(Memory, 71, "FMimallocAllocator heap creation failed");
    }

    m_Heap = Heap;
    m_Generation = AllocateGeneration();
    m_HardBudgetBytes = BudgetBytes;
    m_BudgetReservedBytes.Store(0u, EMemoryOrder::Release);
    m_RequestedBytes.Store(0u, EMemoryOrder::Release);
    m_PeakRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_AllocationCount.Store(0u, EMemoryOrder::Release);
    m_SmallAllocationCount.Store(0u, EMemoryOrder::Release);
    m_SmallRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_MediumAllocationCount.Store(0u, EMemoryOrder::Release);
    m_MediumRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_LargeAllocationCount.Store(0u, EMemoryOrder::Release);
    m_LargeRequestedBytes.Store(0u, EMemoryOrder::Release);
    return {};
}

void FMimallocAllocator::Shutdown() noexcept
{
    if (!m_Heap) {
        return;
    }

    const u64 OutstandingAllocationCount = AllocationCount();
    const u64 OutstandingBytes = BytesAllocated();
    if (OutstandingAllocationCount != 0u || OutstandingBytes != 0u) {
        EmitDestroyedWithLiveAllocations(this, OutstandingAllocationCount, OutstandingBytes, m_HardBudgetBytes);
    }

    auto* Heap = static_cast<mi_heap_t*>(m_Heap);
    mi_heap_collect(Heap, true);
    mi_heap_destroy(Heap);

    m_Heap = nullptr;
    m_Generation = 0;
    m_HardBudgetBytes = 0;
    m_BudgetReservedBytes.Store(0u, EMemoryOrder::Release);
    m_RequestedBytes.Store(0u, EMemoryOrder::Release);
    m_PeakRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_AllocationCount.Store(0u, EMemoryOrder::Release);
    m_SmallAllocationCount.Store(0u, EMemoryOrder::Release);
    m_SmallRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_MediumAllocationCount.Store(0u, EMemoryOrder::Release);
    m_MediumRequestedBytes.Store(0u, EMemoryOrder::Release);
    m_LargeAllocationCount.Store(0u, EMemoryOrder::Release);
    m_LargeRequestedBytes.Store(0u, EMemoryOrder::Release);
}

void* FMimallocAllocator::Alloc(usize Size, usize Alignment, FSourceLoc) noexcept
{
    if (!m_Heap || Size == 0u || !IsPow2(Alignment)) {
        return nullptr;
    }

    const usize EffectiveAlignment = Alignment < kMinimumEffectiveAlignment ? kMinimumEffectiveAlignment : Alignment;
    usize AllocationSize = 0;
    if (!CalculateAllocationSize(Size, EffectiveAlignment, AllocationSize) ||
        !TryReserveBudget(static_cast<u64>(Size))) {
        return nullptr;
    }

    void* AllocationBase = mi_heap_malloc(static_cast<mi_heap_t*>(m_Heap), AllocationSize);
    if (!AllocationBase) {
        ReleaseBudget(static_cast<u64>(Size));
        return nullptr;
    }

    void* UserPointer = InitializeAllocationMetadata(AllocationBase, this, m_Generation, Size, EffectiveAlignment);
    RecordAllocation(static_cast<u64>(Size));
    return UserPointer;
}

void FMimallocAllocator::Free(void* Pointer) noexcept
{
    if (!Pointer) {
        return;
    }

    const AllocationHeader* ConstHeader = ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer);
    if (!ConstHeader) {
        return;
    }

    auto* Header = const_cast<AllocationHeader*>(ConstHeader);
    const u64 RequestedBytes = Header->RequestedBytes;
    auto* Prefix = const_cast<AllocationPrefix*>(PrefixFromUserPointer(Pointer));
    void* AllocationBase = Prefix->AllocationBase;

    Header->Magic = 0;
    Prefix->Magic = 0;
    RecordFree(RequestedBytes);
    ReleaseBudget(RequestedBytes);
    mi_free(AllocationBase);
}

void* FMimallocAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                                  FSourceLoc Location) noexcept
{
    (void)OldSize;
    if (!Pointer) {
        return Alloc(NewSize, Alignment, Location);
    }
    if (!m_Heap) {
        return NewSize == 0u ? Pointer : nullptr;
    }

    const AllocationHeader* OldHeader = ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer);
    if (!OldHeader) {
        return NewSize == 0u ? Pointer : nullptr;
    }
    if (NewSize == 0u) {
        Free(Pointer);
        return nullptr;
    }
    if (!IsPow2(Alignment)) {
        return nullptr;
    }

    const u64 OldRequestedBytes = OldHeader->RequestedBytes;
    const u64 NewRequestedBytes = static_cast<u64>(NewSize);
    const usize EffectiveAlignment = Alignment < kMinimumEffectiveAlignment ? kMinimumEffectiveAlignment : Alignment;

    if (OldRequestedBytes == NewRequestedBytes && (reinterpret_cast<uptr>(Pointer) & (EffectiveAlignment - 1u)) == 0u) {
        return Pointer;
    }

    usize AllocationSize = 0;
    if (!CalculateAllocationSize(NewSize, EffectiveAlignment, AllocationSize)) {
        return nullptr;
    }
    // 新旧ブロックが同時に存在する確保処理中もハード予算へ含める。
    if (!TryReserveBudget(NewRequestedBytes)) {
        return nullptr;
    }

    void* NewAllocationBase = mi_heap_malloc(static_cast<mi_heap_t*>(m_Heap), AllocationSize);
    if (!NewAllocationBase) {
        ReleaseBudget(NewRequestedBytes);
        return nullptr;
    }

    void* NewPointer = InitializeAllocationMetadata(NewAllocationBase, this, m_Generation, NewSize, EffectiveAlignment);
    const usize CopySize = OldRequestedBytes < NewRequestedBytes ? static_cast<usize>(OldRequestedBytes) : NewSize;
    MemCopy(NewPointer, Pointer, CopySize);

    auto* MutableOldHeader = const_cast<AllocationHeader*>(OldHeader);
    auto* OldPrefix = const_cast<AllocationPrefix*>(PrefixFromUserPointer(Pointer));
    void* OldAllocationBase = OldPrefix->AllocationBase;
    MutableOldHeader->Magic = 0;
    OldPrefix->Magic = 0;

    RecordReallocation(OldRequestedBytes, NewRequestedBytes);
    mi_free(OldAllocationBase);
    ReleaseBudget(OldRequestedBytes);
    return NewPointer;
}

bool FMimallocAllocator::OwnsAllocation(const void* Pointer) const noexcept
{
    return ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer) != nullptr;
}

void FMimallocAllocator::Collect(bool bForce) noexcept
{
    if (m_Heap) {
        mi_heap_collect(static_cast<mi_heap_t*>(m_Heap), bForce);
    }
}

MimallocAllocationHistogram FMimallocAllocator::CaptureAllocationHistogram() const noexcept
{
    MimallocAllocationHistogram Histogram;
    Histogram.small.allocation_count = m_SmallAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.small.requested_bytes = m_SmallRequestedBytes.Load(EMemoryOrder::Acquire);
    Histogram.medium.allocation_count = m_MediumAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.medium.requested_bytes = m_MediumRequestedBytes.Load(EMemoryOrder::Acquire);
    Histogram.large.allocation_count = m_LargeAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.large.requested_bytes = m_LargeRequestedBytes.Load(EMemoryOrder::Acquire);
    return Histogram;
}

MimallocHeapInspectionStatistics FMimallocAllocator::InspectHeap() noexcept
{
    MimallocHeapInspectionStatistics Empty;
    if (!m_Heap) {
        return Empty;
    }

    HeapInspectionContext Context;
    Context.Owner = this;
    Context.Generation = m_Generation;
    Context.Statistics.metadata_valid = true;
    Context.Statistics.visit_succeeded = mi_heap_visit_blocks(static_cast<mi_heap_t*>(m_Heap), true, &VisitHeapBlock,
                                                              &Context);
    Context.Statistics.matches_authoritative_statistics = Context.Statistics.visit_succeeded &&
                                                          Context.Statistics.metadata_valid &&
                                                          Context.Statistics.allocation_count == AllocationCount() &&
                                                          Context.Statistics.requested_bytes == BytesAllocated();
    return Context.Statistics;
}

int FMimallocAllocator::RuntimeVersion() noexcept
{
    ConfigureMimallocProcessOnce();
    return mi_version();
}

bool FMimallocAllocator::TryReserveBudget(u64 Amount) noexcept
{
    if (Amount == 0u) {
        return true;
    }

    u64 Current = m_BudgetReservedBytes.Load(EMemoryOrder::Acquire);
    for (;;) {
        const u64 Maximum = ~u64(0);
        if (Amount > Maximum - Current) {
            return false;
        }
        const u64 Desired = Current + Amount;
        if (m_HardBudgetBytes != 0u && Desired > m_HardBudgetBytes) {
            return false;
        }
        if (m_BudgetReservedBytes.CompareExchange(Current, Desired)) {
            return true;
        }
    }
}

void FMimallocAllocator::ReleaseBudget(u64 Amount) noexcept
{
    if (Amount != 0u) {
        m_BudgetReservedBytes.FetchSub(Amount);
    }
}

void FMimallocAllocator::RecordAllocation(u64 RequestedBytes) noexcept
{
    const u64 Previous = m_RequestedBytes.FetchAdd(RequestedBytes);
    m_AllocationCount.FetchAdd(1u);
    UpdatePeak(Previous + RequestedBytes);

    if (RequestedBytes <= kSmallAllocationMaximumBytes) {
        m_SmallAllocationCount.FetchAdd(1u);
        m_SmallRequestedBytes.FetchAdd(RequestedBytes);
    } else if (RequestedBytes <= kMediumAllocationMaximumBytes) {
        m_MediumAllocationCount.FetchAdd(1u);
        m_MediumRequestedBytes.FetchAdd(RequestedBytes);
    } else {
        m_LargeAllocationCount.FetchAdd(1u);
        m_LargeRequestedBytes.FetchAdd(RequestedBytes);
    }
}

void FMimallocAllocator::RecordFree(u64 RequestedBytes) noexcept
{
    m_RequestedBytes.FetchSub(RequestedBytes);
    m_AllocationCount.FetchSub(1u);

    if (RequestedBytes <= kSmallAllocationMaximumBytes) {
        m_SmallAllocationCount.FetchSub(1u);
        m_SmallRequestedBytes.FetchSub(RequestedBytes);
    } else if (RequestedBytes <= kMediumAllocationMaximumBytes) {
        m_MediumAllocationCount.FetchSub(1u);
        m_MediumRequestedBytes.FetchSub(RequestedBytes);
    } else {
        m_LargeAllocationCount.FetchSub(1u);
        m_LargeRequestedBytes.FetchSub(RequestedBytes);
    }
}

void FMimallocAllocator::RecordReallocation(u64 OldRequestedBytes, u64 NewRequestedBytes) noexcept
{
    if (NewRequestedBytes > OldRequestedBytes) {
        const u64 Growth = NewRequestedBytes - OldRequestedBytes;
        const u64 Previous = m_RequestedBytes.FetchAdd(Growth);
        UpdatePeak(Previous + Growth);
    } else if (NewRequestedBytes < OldRequestedBytes) {
        m_RequestedBytes.FetchSub(OldRequestedBytes - NewRequestedBytes);
    }

    if (OldRequestedBytes <= kSmallAllocationMaximumBytes) {
        m_SmallAllocationCount.FetchSub(1u);
        m_SmallRequestedBytes.FetchSub(OldRequestedBytes);
    } else if (OldRequestedBytes <= kMediumAllocationMaximumBytes) {
        m_MediumAllocationCount.FetchSub(1u);
        m_MediumRequestedBytes.FetchSub(OldRequestedBytes);
    } else {
        m_LargeAllocationCount.FetchSub(1u);
        m_LargeRequestedBytes.FetchSub(OldRequestedBytes);
    }

    if (NewRequestedBytes <= kSmallAllocationMaximumBytes) {
        m_SmallAllocationCount.FetchAdd(1u);
        m_SmallRequestedBytes.FetchAdd(NewRequestedBytes);
    } else if (NewRequestedBytes <= kMediumAllocationMaximumBytes) {
        m_MediumAllocationCount.FetchAdd(1u);
        m_MediumRequestedBytes.FetchAdd(NewRequestedBytes);
    } else {
        m_LargeAllocationCount.FetchAdd(1u);
        m_LargeRequestedBytes.FetchAdd(NewRequestedBytes);
    }
}

void FMimallocAllocator::UpdatePeak(u64 Candidate) noexcept
{
    u64 Current = m_PeakRequestedBytes.Load(EMemoryOrder::Acquire);
    while (Candidate > Current) {
        if (m_PeakRequestedBytes.CompareExchange(Current, Candidate)) {
            return;
        }
    }
}

} // namespace acs
