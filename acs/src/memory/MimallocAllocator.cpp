// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - mimalloc first-class heap アロケータ実装
// =============================================================================
#include "memory/MimallocAllocator.h"

#include "foundation/Platform.h"
#include "foundation/Assert.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"

#include <mimalloc.h>

namespace acs {

namespace {

/** 全 CMimallocAllocator に共通する mimalloc プロセス設定の初期化状態。 */
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

/** 猶予リストを自動回収する解放済みブロック件数。 */
constexpr u64 kRetiredAllocationCountThreshold = 64u;

/** 猶予リストを自動回収する要求バイト合計。 */
constexpr u64 kRetiredRequestedBytesThreshold = 1024u * 1024u;

/** 最初の arena 予約を 64 MiB に抑え、巨大な仮想予約を既定にしない。値の単位は KiB。 */
constexpr long kArenaReserveKibibytes = 64 * 1024;

/**
 * mimalloc の生ブロック先頭に置く追跡ヘッダ。
 *
 * @details ブロック列挙時はこのヘッダだけで要求量と所有者を復元できる。
 */
enum class EAllocationState : u32
{
    Active = 1u,
    Reallocating = 2u,
    Released = 3u,
};

struct FAllocationHeader
{
    /** Active / Reallocating / Released を原子的に遷移させる。 */
    alignas(sizeof(u32)) volatile u32 State = 0u;

    u64 Magic = 0;
    const CMimallocAllocator* Owner = nullptr;
    u64 Generation = 0;
    u64 RequestedBytes = 0;
    usize EffectiveAlignment = 0;
    usize UserOffset = 0;

    /** Released 後、物理解放まで使う侵入リストの次要素。 */
    FAllocationHeader* RetiredNext = nullptr;
};

/** 利用者ポインタから mimalloc 生ブロックへ戻るための固定長プレフィックス。 */
struct FAllocationPrefix {
    void* AllocationBase = nullptr;
    u64 Magic = 0;
};

static_assert((sizeof(FAllocationPrefix) % alignof(void*)) == 0, "AllocationPrefix must preserve pointer alignment");

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
    const usize FixedOverhead = sizeof(FAllocationHeader) + sizeof(FAllocationPrefix);
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
void* InitializeAllocationMetadata(void* AllocationBase, CMimallocAllocator* Owner, u64 Generation, usize RequestedSize,
                                   usize EffectiveAlignment) noexcept
{
    auto* Header = static_cast<FAllocationHeader*>(AllocationBase);
    const uptr FirstUserAddress = reinterpret_cast<uptr>(AllocationBase) + sizeof(FAllocationHeader) +
                                  sizeof(FAllocationPrefix);
    const uptr UserAddress = AlignUp(FirstUserAddress, EffectiveAlignment);
    auto* Prefix = reinterpret_cast<FAllocationPrefix*>(UserAddress - sizeof(FAllocationPrefix));

    Header->Magic = kAllocationHeaderMagic;
    Header->Owner = Owner;
    Header->Generation = Generation;
    Header->RequestedBytes = static_cast<u64>(RequestedSize);
    Header->EffectiveAlignment = EffectiveAlignment;
    Header->UserOffset = static_cast<usize>(UserAddress - reinterpret_cast<uptr>(AllocationBase));
    Header->RetiredNext = nullptr;

    Prefix->AllocationBase = AllocationBase;
    Prefix->Magic = kAllocationPrefixMagic;
    atomic_detail::StoreRelease(&Header->State, static_cast<u32>(EAllocationState::Active));
    return reinterpret_cast<void*>(UserAddress);
}

/** 所有者、世代、位置、アライメントを含めてヘッダとプレフィックスを検証する。 */
const FAllocationHeader* ValidateAllocationMetadata(const CMimallocAllocator* Owner, const void* Heap, u64 Generation,
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
    if ((PointerAddress & (alignof(void*) - 1u)) != 0u || PointerAddress < sizeof(FAllocationPrefix)) {
        return nullptr;
    }

    const uptr PrefixAddress = PointerAddress - sizeof(FAllocationPrefix);
    const auto* Prefix = reinterpret_cast<const FAllocationPrefix*>(PrefixAddress);
    if (!mi_heap_contains(MimallocHeap, Prefix)) {
        return nullptr;
    }
    if (Prefix->Magic != kAllocationPrefixMagic || !Prefix->AllocationBase) {
        return nullptr;
    }
    if ((reinterpret_cast<uptr>(Prefix->AllocationBase) & (alignof(FAllocationHeader) - 1u)) != 0u) {
        return nullptr;
    }
    if (!mi_heap_contains(MimallocHeap, Prefix->AllocationBase)) {
        return nullptr;
    }

    const auto* Header = static_cast<const FAllocationHeader*>(Prefix->AllocationBase);
    if (atomic_detail::LoadAcquire(&Header->State) != static_cast<u32>(EAllocationState::Active) ||
        Header->Magic != kAllocationHeaderMagic || Header->Owner != Owner || Header->Generation != Generation ||
        Header->RequestedBytes == 0u || !IsPow2(Header->EffectiveAlignment) ||
        Header->EffectiveAlignment < kMinimumEffectiveAlignment) {
        return nullptr;
    }

    const usize UsableSize = mi_usable_size(Prefix->AllocationBase);
    const usize MinimumUserOffset = sizeof(FAllocationHeader) + sizeof(FAllocationPrefix);
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
void EmitDestroyedWithLiveAllocations(const CMimallocAllocator* Allocator, u64 OutstandingAllocationCount,
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
struct FHeapInspectionContext {
    const CMimallocAllocator* Owner = nullptr;
    u64 Generation = 0;
    FMimallocHeapInspectionStatistics Statistics = {};
};

/** mi_heap_visit_blocks の領域通知と生存ブロック通知を集計する。 */
bool mi_cdecl VisitHeapBlock(const mi_heap_t*, const mi_heap_area_t* Area, void* Block, size_t BlockSize,
                             void* Argument)
{
    auto* Context = static_cast<FHeapInspectionContext*>(Argument);
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

    if (BlockSize < sizeof(FAllocationHeader)) {
        Context->Statistics.metadata_valid = false;
        return true;
    }

    const auto* Header = static_cast<const FAllocationHeader*>(Block);
    const usize MinimumUserOffset = sizeof(FAllocationHeader) + sizeof(FAllocationPrefix);
    if (atomic_detail::LoadAcquire(&Header->State) != static_cast<u32>(EAllocationState::Active) ||
        Header->Magic != kAllocationHeaderMagic || Header->Owner != Context->Owner ||
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
    const auto* Prefix = reinterpret_cast<const FAllocationPrefix*>(UserAddress - sizeof(FAllocationPrefix));
    if ((UserAddress & (Header->EffectiveAlignment - 1u)) != 0u || Prefix->Magic != kAllocationPrefixMagic ||
        Prefix->AllocationBase != Block) {
        Context->Statistics.metadata_valid = false;
        return true;
    }

    Context->Statistics.requested_bytes = SaturatingAdd(Context->Statistics.requested_bytes, Header->RequestedBytes);
    return true;
}

} // namespace

/** 公開操作の入場登録をスコープ終了時に必ず解除する。 */
class CMimallocAllocator::FLifecycleOperation final
{
public:
    explicit FLifecycleOperation(const CMimallocAllocator& Allocator) noexcept
        : m_Allocator(&Allocator)
        , m_bAdmitted(Allocator.TryBeginLifecycleOperation())
    {
    }

    ~FLifecycleOperation() noexcept
    {
        if (m_bAdmitted)
        {
            m_Allocator->EndLifecycleOperation();
        }
    }

    FLifecycleOperation(const FLifecycleOperation&) = delete;
    FLifecycleOperation& operator=(const FLifecycleOperation&) = delete;

    bool WasAdmitted() const noexcept
    {
        return m_bAdmitted;
    }

private:
    /** 入場先アロケータ。ガードより長く生存する。 */
    const CMimallocAllocator* m_Allocator = nullptr;

    /** Running 中の公開操作として登録済みなら true。 */
    bool m_bAdmitted = false;
};

bool CMimallocAllocator::TryBeginLifecycleOperation() const noexcept
{
    u64 GateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    const u64 EntryGeneration = GateValue & kLifecycleGenerationMask;
    while ((GateValue & kLifecycleAcceptingBit) != 0u)
    {
        if ((GateValue & kLifecycleGenerationMask) != EntryGeneration)
        {
            // 保守停止または再初期化をまたいだ古い CAS を新しい受付世代へ通さない。
            return false;
        }

        const u64 OperationCount = GateValue & kLifecycleOperationCountMask;
        if (OperationCount == kLifecycleOperationCountMask)
        {
            ACS_ASSERT(false && "CMimallocAllocator: lifecycle operation count overflow");
            return false;
        }

        const u64 DesiredGateValue = GateValue + 1u;
        if (m_LifecycleGate.CompareExchange(GateValue, DesiredGateValue))
        {
            return true;
        }
    }
    return false;
}

void CMimallocAllocator::EndLifecycleOperation() const noexcept
{
    u64 GateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    for (;;)
    {
        const u64 OperationCount = GateValue & kLifecycleOperationCountMask;
        ACS_ASSERT(OperationCount > 0u);

        if ((GateValue & kLifecycleAcceptingBit) == 0u && OperationCount == 1u)
        {
            FScopedLock DrainLock(m_LifecycleDrainLock);
            u64 ExpectedGateValue = GateValue;
            if (!m_LifecycleGate.CompareExchange(ExpectedGateValue, GateValue - 1u))
            {
                GateValue = ExpectedGateValue;
                continue;
            }
            m_LifecycleDrainedCondition.NotifyAll();
            return;
        }

        u64 ExpectedGateValue = GateValue;
        if (m_LifecycleGate.CompareExchange(ExpectedGateValue, GateValue - 1u))
        {
            return;
        }
        GateValue = ExpectedGateValue;
    }
}

void CMimallocAllocator::CloseLifecycleGateAndWait() noexcept
{
    m_LifecycleGate.FetchAnd(~kLifecycleAcceptingBit);

    FScopedLock DrainLock(m_LifecycleDrainLock);
    while ((m_LifecycleGate.Load(EMemoryOrder::Acquire) & kLifecycleOperationCountMask) != 0u)
    {
        m_LifecycleDrainedCondition.Wait(m_LifecycleDrainLock);
    }
}

void CMimallocAllocator::OpenLifecycleGate() noexcept
{
    const u64 ClosedGateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    ACS_ASSERT((ClosedGateValue & kLifecycleAcceptingBit) == 0u);
    ACS_ASSERT((ClosedGateValue & kLifecycleOperationCountMask) == 0u);
    ACS_ASSERT(m_Heap != nullptr && m_Generation != 0u);

    u64 NextGeneration = (ClosedGateValue + kLifecycleGenerationIncrement) & kLifecycleGenerationMask;
    if (NextGeneration == 0u)
    {
        NextGeneration = kLifecycleGenerationIncrement;
    }
    m_LifecycleGate.Store(kLifecycleAcceptingBit | NextGeneration, EMemoryOrder::Release);
}

CMimallocAllocator::~CMimallocAllocator() noexcept
{
    Shutdown();
}

TResult<void> CMimallocAllocator::Init(u64 BudgetBytes) noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    if (m_Heap)
    {
        return ACS_ERR(Memory, 70, "CMimallocAllocator already initialized");
    }

    ConfigureMimallocProcessOnce();
    mi_heap_t* Heap = mi_heap_new();
    if (!Heap) {
        return ACS_ERR(Memory, 71, "CMimallocAllocator heap creation failed");
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
    {
        FScopedLock RetiredAllocationLock(m_RetiredAllocationLock);
        m_RetiredAllocationHead = nullptr;
        m_RetiredAllocationCount = 0u;
        m_RetiredRequestedBytes = 0u;
        m_RetiredCollectionRequested.Store(0u, EMemoryOrder::Release);
    }
    OpenLifecycleGate();
    return {};
}

void CMimallocAllocator::Shutdown() noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    CloseLifecycleGateAndWait();
    if (!m_Heap)
    {
        return;
    }

    const u64 OutstandingAllocationCount = m_AllocationCount.Load(EMemoryOrder::Acquire);
    const u64 OutstandingBytes = m_RequestedBytes.Load(EMemoryOrder::Acquire);
    if (OutstandingAllocationCount != 0u || OutstandingBytes != 0u)
    {
        EmitDestroyedWithLiveAllocations(this, OutstandingAllocationCount, OutstandingBytes, m_HardBudgetBytes);
    }

    auto* Heap = static_cast<mi_heap_t*>(m_Heap);
    FreeRetiredAllocationsWhileLifecycleIsPaused();
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

void* CMimallocAllocator::Alloc(usize Size, usize Alignment, FSourceLoc) noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return nullptr;
    }
    return AllocateAfterLifecycleAdmission(Size, Alignment);
}

void* CMimallocAllocator::AllocateAfterLifecycleAdmission(usize Size, usize Alignment) noexcept
{
    if (!m_Heap || Size == 0u || !IsPow2(Alignment))
    {
        return nullptr;
    }

    const usize EffectiveAlignment = Alignment < kMinimumEffectiveAlignment ? kMinimumEffectiveAlignment : Alignment;
    usize AllocationSize = 0u;
    if (!CalculateAllocationSize(Size, EffectiveAlignment, AllocationSize) ||
        !TryReserveBudget(static_cast<u64>(Size)))
    {
        return nullptr;
    }

    void* AllocationBase = nullptr;
    {
        FScopedLock HeapLock(m_HeapLock);
        AllocationBase = mi_heap_malloc(static_cast<mi_heap_t*>(m_Heap), AllocationSize);
    }
    if (!AllocationBase)
    {
        ReleaseBudget(static_cast<u64>(Size));
        return nullptr;
    }

    void* const UserPointer =
        InitializeAllocationMetadata(AllocationBase, this, m_Generation, Size, EffectiveAlignment);
    RecordAllocation(static_cast<u64>(Size));
    return UserPointer;
}

void CMimallocAllocator::Free(void* Pointer) noexcept
{
    (void)TryFreeAllocation(Pointer);
}

bool CMimallocAllocator::TryFreeAllocation(void* Pointer) noexcept
{
    if (!Pointer)
    {
        return false;
    }

    bool bCollectionRequested = false;
    bool bReleased = false;
    {
        FLifecycleOperation LifecycleOperation(*this);
        if (!LifecycleOperation.WasAdmitted())
        {
            return false;
        }
        bReleased = FreeAfterLifecycleAdmission(Pointer, bCollectionRequested);
    }

    // 自分の admission を解除してから停止する。入場中に drain を待つと自己デッドロックになる。
    if (bCollectionRequested)
    {
        CollectRetiredAllocationsIfNeeded();
    }
    return bReleased;
}

bool CMimallocAllocator::FreeAfterLifecycleAdmission(void* Pointer, bool& bCollectionRequested) noexcept
{
    bCollectionRequested = false;
    const FAllocationHeader* ConstHeader = nullptr;
    {
        FScopedLock HeapLock(m_HeapLock);
        ConstHeader = ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer);
    }
    if (!ConstHeader)
    {
        return false;
    }

    auto* const Header = const_cast<FAllocationHeader*>(ConstHeader);
    u32 ExpectedState = static_cast<u32>(EAllocationState::Active);
    if (!atomic_detail::CompareExchange(&Header->State, ExpectedState,
                                        static_cast<u32>(EAllocationState::Released)))
    {
        return false;
    }

    const u64 RequestedBytes = Header->RequestedBytes;
    void* const AllocationBase = const_cast<void*>(static_cast<const void*>(Header));
    RecordFree(RequestedBytes);
    ReleaseBudget(RequestedBytes);
    bCollectionRequested = RetireAllocation(AllocationBase, RequestedBytes);
    return true;
}

void* CMimallocAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                                  FSourceLoc Location) noexcept
{
    (void)OldSize;
    (void)Location;
    const FMimallocReallocationResult Result = TryReallocateAllocation(Pointer, NewSize, Alignment);
    if (Pointer && NewSize == 0u && !Result.bOwnershipAccepted)
    {
        return Pointer;
    }
    return Result.Pointer;
}

FMimallocReallocationResult CMimallocAllocator::TryReallocateAllocation(void* Pointer, usize NewSize,
                                                                       usize Alignment) noexcept
{
    bool bCollectionRequested = false;
    FMimallocReallocationResult Result;
    {
        FLifecycleOperation LifecycleOperation(*this);
        if (!LifecycleOperation.WasAdmitted())
        {
            return Result;
        }
        Result = ReallocateAfterLifecycleAdmission(Pointer, NewSize, Alignment, bCollectionRequested);
    }

    if (bCollectionRequested)
    {
        CollectRetiredAllocationsIfNeeded();
    }
    return Result;
}

FMimallocReallocationResult CMimallocAllocator::ReallocateAfterLifecycleAdmission(
    void* Pointer, usize NewSize, usize Alignment, bool& bCollectionRequested) noexcept
{
    FMimallocReallocationResult Result;
    bCollectionRequested = false;
    if (!Pointer)
    {
        Result.Pointer = AllocateAfterLifecycleAdmission(NewSize, Alignment);
        Result.bSucceeded = Result.Pointer != nullptr;
        return Result;
    }
    if (!m_Heap || (NewSize != 0u && !IsPow2(Alignment)))
    {
        return Result;
    }

    const FAllocationHeader* ConstOldHeader = nullptr;
    {
        FScopedLock HeapLock(m_HeapLock);
        ConstOldHeader = ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer);
    }
    if (!ConstOldHeader)
    {
        return Result;
    }

    auto* const OldHeader = const_cast<FAllocationHeader*>(ConstOldHeader);
    u32 ExpectedState = static_cast<u32>(EAllocationState::Active);
    if (!atomic_detail::CompareExchange(&OldHeader->State, ExpectedState,
                                        static_cast<u32>(EAllocationState::Reallocating)))
    {
        return Result;
    }
    Result.bOwnershipAccepted = true;

    const u64 OldRequestedBytes = OldHeader->RequestedBytes;
    void* const OldAllocationBase = const_cast<void*>(static_cast<const void*>(OldHeader));
    if (NewSize == 0u)
    {
        atomic_detail::StoreRelease(&OldHeader->State, static_cast<u32>(EAllocationState::Released));
        RecordFree(OldRequestedBytes);
        ReleaseBudget(OldRequestedBytes);
        bCollectionRequested = RetireAllocation(OldAllocationBase, OldRequestedBytes);
        Result.bSucceeded = true;
        return Result;
    }

    const u64 NewRequestedBytes = static_cast<u64>(NewSize);
    const usize EffectiveAlignment = Alignment < kMinimumEffectiveAlignment ? kMinimumEffectiveAlignment : Alignment;
    if (OldRequestedBytes == NewRequestedBytes &&
        (reinterpret_cast<uptr>(Pointer) & (EffectiveAlignment - 1u)) == 0u)
    {
        atomic_detail::StoreRelease(&OldHeader->State, static_cast<u32>(EAllocationState::Active));
        Result.Pointer = Pointer;
        Result.bSucceeded = true;
        return Result;
    }

    usize AllocationSize = 0u;
    if (!CalculateAllocationSize(NewSize, EffectiveAlignment, AllocationSize) ||
        !TryReserveBudget(NewRequestedBytes))
    {
        atomic_detail::StoreRelease(&OldHeader->State, static_cast<u32>(EAllocationState::Active));
        return Result;
    }

    void* NewAllocationBase = nullptr;
    {
        FScopedLock HeapLock(m_HeapLock);
        NewAllocationBase = mi_heap_malloc(static_cast<mi_heap_t*>(m_Heap), AllocationSize);
    }
    if (!NewAllocationBase)
    {
        ReleaseBudget(NewRequestedBytes);
        atomic_detail::StoreRelease(&OldHeader->State, static_cast<u32>(EAllocationState::Active));
        return Result;
    }

    void* const NewPointer =
        InitializeAllocationMetadata(NewAllocationBase, this, m_Generation, NewSize, EffectiveAlignment);
    const usize CopySize = OldRequestedBytes < NewRequestedBytes ? static_cast<usize>(OldRequestedBytes) : NewSize;
    MemCopy(NewPointer, Pointer, CopySize);

    RecordReallocation(OldRequestedBytes, NewRequestedBytes);
    ReleaseBudget(OldRequestedBytes);
    atomic_detail::StoreRelease(&OldHeader->State, static_cast<u32>(EAllocationState::Released));
    bCollectionRequested = RetireAllocation(OldAllocationBase, OldRequestedBytes);

    Result.Pointer = NewPointer;
    Result.bSucceeded = true;
    return Result;
}

u64 CMimallocAllocator::BytesAllocated() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() ? m_RequestedBytes.Load(EMemoryOrder::Acquire) : 0u;
}

u64 CMimallocAllocator::AllocationCount() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() ? m_AllocationCount.Load(EMemoryOrder::Acquire) : 0u;
}

u64 CMimallocAllocator::PeakBytes() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() ? m_PeakRequestedBytes.Load(EMemoryOrder::Acquire) : 0u;
}

u64 CMimallocAllocator::LifetimeGeneration() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() ? m_Generation : 0u;
}

bool CMimallocAllocator::IsInitialized() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() && m_Heap != nullptr;
}

u64 CMimallocAllocator::HardBudgetBytes() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    return LifecycleOperation.WasAdmitted() ? m_HardBudgetBytes : 0u;
}

bool CMimallocAllocator::RetireAllocation(void* AllocationBase, u64 RequestedBytes) noexcept
{
    auto* const Header = static_cast<FAllocationHeader*>(AllocationBase);
    FScopedLock RetiredAllocationLock(m_RetiredAllocationLock);
    Header->RetiredNext = static_cast<FAllocationHeader*>(m_RetiredAllocationHead);
    m_RetiredAllocationHead = Header;
    ++m_RetiredAllocationCount;
    m_RetiredRequestedBytes = SaturatingAdd(m_RetiredRequestedBytes, RequestedBytes);

    const bool bCollectionRequested =
        m_RetiredAllocationCount >= kRetiredAllocationCountThreshold ||
        m_RetiredRequestedBytes >= kRetiredRequestedBytesThreshold;
    if (bCollectionRequested)
    {
        m_RetiredCollectionRequested.Store(1u, EMemoryOrder::Release);
    }
    return bCollectionRequested;
}

void CMimallocAllocator::CollectRetiredAllocationsIfNeeded() noexcept
{
    if (m_RetiredCollectionRequested.Load(EMemoryOrder::Acquire) == 0u)
    {
        return;
    }

    // この関数へ入る前に呼び出し元の admission は解除済み。control 保持中に受付を閉じ、
    // 重なって入場済みの全操作がヘッダ検証/CASを終えてからだけ物理解放する。
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    if (!m_Heap || m_RetiredCollectionRequested.Load(EMemoryOrder::Acquire) == 0u)
    {
        return;
    }

    CloseLifecycleGateAndWait();
    FreeRetiredAllocationsWhileLifecycleIsPaused();
    mi_heap_collect(static_cast<mi_heap_t*>(m_Heap), false);
    OpenLifecycleGate();
}

void CMimallocAllocator::FreeRetiredAllocationsWhileLifecycleIsPaused() noexcept
{
    FScopedLock RetiredAllocationLock(m_RetiredAllocationLock);
    auto* Header = static_cast<FAllocationHeader*>(m_RetiredAllocationHead);
    m_RetiredAllocationHead = nullptr;
    m_RetiredAllocationCount = 0u;
    m_RetiredRequestedBytes = 0u;
    m_RetiredCollectionRequested.Store(0u, EMemoryOrder::Release);

    while (Header)
    {
        FAllocationHeader* const NextHeader = Header->RetiredNext;
        mi_free(Header);
        Header = NextHeader;
    }
}

bool CMimallocAllocator::OwnsAllocation(const void* Pointer) const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return false;
    }
    FScopedLock HeapLock(m_HeapLock);
    return ValidateAllocationMetadata(this, m_Heap, m_Generation, Pointer) != nullptr;
}

void CMimallocAllocator::Collect(bool bForce) noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    CloseLifecycleGateAndWait();
    if (!m_Heap)
    {
        return;
    }

    FreeRetiredAllocationsWhileLifecycleIsPaused();
    mi_heap_collect(static_cast<mi_heap_t*>(m_Heap), bForce);
    OpenLifecycleGate();
}

FMimallocAllocationHistogram CMimallocAllocator::CaptureAllocationHistogram() const noexcept
{
    FMimallocAllocationHistogram Histogram;
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return Histogram;
    }

    Histogram.small.allocation_count = m_SmallAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.small.requested_bytes = m_SmallRequestedBytes.Load(EMemoryOrder::Acquire);
    Histogram.medium.allocation_count = m_MediumAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.medium.requested_bytes = m_MediumRequestedBytes.Load(EMemoryOrder::Acquire);
    Histogram.large.allocation_count = m_LargeAllocationCount.Load(EMemoryOrder::Acquire);
    Histogram.large.requested_bytes = m_LargeRequestedBytes.Load(EMemoryOrder::Acquire);
    return Histogram;
}

FMimallocHeapInspectionStatistics CMimallocAllocator::InspectHeap() noexcept
{
    FMimallocHeapInspectionStatistics Empty;
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    CloseLifecycleGateAndWait();
    if (!m_Heap)
    {
        return Empty;
    }

    FreeRetiredAllocationsWhileLifecycleIsPaused();
    FHeapInspectionContext Context;
    Context.Owner = this;
    Context.Generation = m_Generation;
    Context.Statistics.metadata_valid = true;
    Context.Statistics.visit_succeeded = mi_heap_visit_blocks(static_cast<mi_heap_t*>(m_Heap), true, &VisitHeapBlock,
                                                              &Context);
    Context.Statistics.matches_authoritative_statistics = Context.Statistics.visit_succeeded &&
                                                           Context.Statistics.metadata_valid &&
                                                           Context.Statistics.allocation_count ==
                                                               m_AllocationCount.Load(EMemoryOrder::Acquire) &&
                                                           Context.Statistics.requested_bytes ==
                                                               m_RequestedBytes.Load(EMemoryOrder::Acquire);
    OpenLifecycleGate();
    return Context.Statistics;
}

int CMimallocAllocator::RuntimeVersion() noexcept
{
    ConfigureMimallocProcessOnce();
    return mi_version();
}

bool CMimallocAllocator::TryReserveBudget(u64 Amount) noexcept
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

void CMimallocAllocator::ReleaseBudget(u64 Amount) noexcept
{
    if (Amount != 0u) {
        m_BudgetReservedBytes.FetchSub(Amount);
    }
}

void CMimallocAllocator::RecordAllocation(u64 RequestedBytes) noexcept
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

void CMimallocAllocator::RecordFree(u64 RequestedBytes) noexcept
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

void CMimallocAllocator::RecordReallocation(u64 OldRequestedBytes, u64 NewRequestedBytes) noexcept
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

void CMimallocAllocator::UpdatePeak(u64 Candidate) noexcept
{
    u64 Current = m_PeakRequestedBytes.Load(EMemoryOrder::Acquire);
    while (Candidate > Current) {
        if (m_PeakRequestedBytes.CompareExchange(Current, Candidate)) {
            return;
        }
    }
}

} // namespace acs
