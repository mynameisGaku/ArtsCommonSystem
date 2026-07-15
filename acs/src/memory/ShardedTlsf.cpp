// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FShardedTlsfAllocator 実装
// =============================================================================
#include "memory/ShardedTlsf.h"
#include "memory/VirtualMemory.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"
#include "foundation/Compiler.h"
#include "foundation/Log.h"
#include "foundation/Platform.h" // GetSystemInfo

namespace acs {

namespace {

/**
 * 論理コア数を返す (シャード数の自動決定 / clamp 用)。
 *
 * @return 論理プロセッサ数 (0 を返さないよう最低 1)。
 */
u32 DetectLogicalCores() noexcept
{
    SYSTEM_INFO SystemInfo{};
    ::GetSystemInfo(&SystemInfo);
    const u32 LogicalCoreCount = static_cast<u32>(SystemInfo.dwNumberOfProcessors);
    return LogicalCoreCount > 0u ? LogicalCoreCount : 1u;
}

/** スレッドに割り当てるシャード index。全シャード化アロケータで共有する分散ヒント (各々 m_ShardCount で剰余)。 */
ACS_THREAD_LOCAL int t_assigned_shard = -1;

/** thread-local マガジンのサイズクラス数 (block_size 24..520 をカバー)。 */
constexpr u32 kCacheClasses = 32;

/** 1 サイズクラスあたりの最大キャッシュ数。 */
constexpr u32 kBucketCap = 64;

/** refill 1 回でまとめて確保するブロック数。 */
constexpr u32 kRefillBatch = 32;

/** キャッシュ対象の最小 block_size (TLSF 量子化の最小ブロック)。 */
constexpr usize kCacheMinBlock = 24;

/** キャッシュ対象の最大 block_size (= 24 + 16×(kCacheClasses-1) = 520)。 */
constexpr usize kCacheMaxBlock = kCacheMinBlock + 16u * (kCacheClasses - 1);

/**
 * block_size をキャッシュのサイズクラスに変換する。
 *
 * @details TLSF の block_size は 24,40,56,... (=24+16k) なので k をクラスにする。範囲外や非量子サイズは対象外。
 * @param BlockSize TLSF の block_size。
 * @return サイズクラス index (対象外は -1)。
 */
ACS_FORCEINLINE int CacheClassOfBlock(usize BlockSize) noexcept
{
    if (BlockSize < kCacheMinBlock || BlockSize > kCacheMaxBlock) return -1;
    const usize Difference = BlockSize - kCacheMinBlock;
    if ((Difference & 15u) != 0u) return -1;
    return static_cast<int>(Difference >> 4);
}

/**
 * 確保要求サイズから TLSF が割り当てる block_size を求める (AdjustRequestSize と同一式)。
 *
 * @param Size 確保要求サイズ。
 * @return 対応する block_size。
 */
ACS_FORCEINLINE usize CacheBlockSizeForRequest(usize Size) noexcept
{
    usize BlockSize = ((Size + 15u) & ~usize(15)) + 8u;
    if (BlockSize < kCacheMinBlock) BlockSize = kCacheMinBlock;
    return BlockSize;
}

/** 2 のべき乗境界への切り上げを、加算オーバーフローなしで行う。 */
bool TryAlignShardSize(usize Value, usize Alignment, usize& AlignedValue) noexcept
{
    if (!IsPow2(Alignment) || Value > (~usize(0)) - (Alignment - 1u)) {
        return false;
    }
    AlignedValue = (Value + Alignment - 1u) & ~(Alignment - 1u);
    return true;
}

} // namespace

namespace memory_detail {

/** owner と世代を保持し、スレッド終了時に生存中の owner へブロックを返すマガジン。 */
struct FShardedTlsfThreadCache {
    /** このマガジンを専有するアロケータ。ポインタは寿命レジストリ照合前に参照しない。 */
    FShardedTlsfAllocator* owner = nullptr;

    /** owner の世代。再 Init 後の同一アドレスを別寿命として区別する。 */
    u64 epoch = 0;

    /**
     * サイズクラスごとのポインタ格納領域。
     *
     * @details 解放済み payload 内へ next ポインタを書かない。UAF 書込みで payload が壊れても、
     * マガジン管理がその値をアドレスとして参照しないため、未コミット領域への誤アクセスを防げる。
     */
    void* entries[kCacheClasses][kBucketCap] = {};

    /** サイズクラスごとのキャッシュ済みブロック数。 */
    u32 count[kCacheClasses] = {};

    /** 指定クラスへポインタを積む。満杯または不正クラスなら false。 */
    bool Push(u32 CacheClass, void* Pointer) noexcept
    {
        if (CacheClass >= kCacheClasses || !Pointer || count[CacheClass] >= kBucketCap) return false;
        entries[CacheClass][count[CacheClass]++] = Pointer;
        return true;
    }

    /** 指定クラスから最後に積んだポインタを取り出す。空なら nullptr。 */
    void* Pop(u32 CacheClass) noexcept
    {
        if (CacheClass >= kCacheClasses || count[CacheClass] == 0u) return nullptr;
        const u32 Index = --count[CacheClass];
        void* const Pointer = entries[CacheClass][Index];
        entries[CacheClass][Index] = nullptr;
        return Pointer;
    }

    /** スレッド終了時に、owner がまだ生存中の場合だけキャッシュを返却する。 */
    ~FShardedTlsfThreadCache() noexcept
    {
        FShardedTlsfAllocator::ReturnThreadCacheIfLifetimeIsActive(*this);
    }

    /** 別 owner または別世代へ切り替える前に、旧キャッシュを安全に返却する。 */
    void PrepareFor(FShardedTlsfAllocator* NextOwner, u64 NextEpoch) noexcept
    {
        if (owner == NextOwner && epoch == NextEpoch) return;
        FShardedTlsfAllocator::ReturnThreadCacheIfLifetimeIsActive(*this);
        owner = NextOwner;
        epoch = NextEpoch;
    }

    /** owner の VM に触れず、マガジンの参照だけを破棄する。 */
    void ClearWithoutReturning() noexcept
    {
        for (u32 CacheClass = 0; CacheClass < kCacheClasses; ++CacheClass) {
            while (count[CacheClass] > 0u) {
                entries[CacheClass][--count[CacheClass]] = nullptr;
            }
        }
        owner = nullptr;
        epoch = 0;
    }
};

} // namespace memory_detail

namespace {

/** 呼び出しスレッドのマガジン。native thread_local によりスレッド終了時のデストラクタを保証する。 */
thread_local memory_detail::FShardedTlsfThreadCache t_thread_cache;

/** Init ごとに新しい世代を配るカウンタ (0 は無効世代)。 */
TAtomic<u64> g_epoch_counter{0};

/** 生存中アロケータの侵入リストを守る。静的破棄を持たない SRWLOCK を使う。 */
SRWLOCK g_thread_cache_registry_lock = SRWLOCK_INIT;

/** thread-local マガジンを受け取れる生存中アロケータの先頭。 */
FShardedTlsfAllocator* g_thread_cache_registry_head = nullptr;

/**
 * ロック取得順は lifecycle control -> registry -> shard とする。
 * lifecycle drain は control 保持中だけ使い、registry より前に解放する。TLS デストラクタは
 * registry -> shard だけを取得し、shard 保持中に control / registry を取得してはならない。
 */

/** 寿命レジストリ用 SRWLOCK のスコープロック。 */
class FThreadCacheRegistryLock final {
public:
    FThreadCacheRegistryLock() noexcept
    {
        ::AcquireSRWLockExclusive(&g_thread_cache_registry_lock);
    }

    ~FThreadCacheRegistryLock() noexcept
    {
        ::ReleaseSRWLockExclusive(&g_thread_cache_registry_lock);
    }

    FThreadCacheRegistryLock(const FThreadCacheRegistryLock&) = delete;
    FThreadCacheRegistryLock& operator=(const FThreadCacheRegistryLock&) = delete;
};

} // namespace

/** 公開操作の入場登録をスコープ終了時に必ず解除する。 */
class FShardedTlsfAllocator::FLifecycleOperation final
{
public:
    explicit FLifecycleOperation(const FShardedTlsfAllocator& Allocator) noexcept
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
    const FShardedTlsfAllocator* m_Allocator = nullptr;

    /** Running 中の公開操作として登録済みなら true。 */
    bool m_bAdmitted = false;
};

bool FShardedTlsfAllocator::TryBeginLifecycleOperation() const noexcept
{
    u64 GateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    const u64 EntryGeneration = GateValue & kLifecycleGenerationMask;
    while ((GateValue & kLifecycleAcceptingBit) != 0u)
    {
        if ((GateValue & kLifecycleGenerationMask) != EntryGeneration)
        {
            // Shutdown をまたいで再初期化された場合、旧世代で始めた入場を新世代へ通さない。
            return false;
        }

        const u64 OperationCount = GateValue & kLifecycleOperationCountMask;
        if (OperationCount == kLifecycleOperationCountMask)
        {
            ACS_ASSERT(false && "FShardedTlsfAllocator: lifecycle operation count overflow");
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

void FShardedTlsfAllocator::EndLifecycleOperation() const noexcept
{
    u64 GateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    for (;;)
    {
        const u64 OperationCount = GateValue & kLifecycleOperationCountMask;
        ACS_ASSERT(OperationCount > 0u);

        if ((GateValue & kLifecycleAcceptingBit) == 0u && OperationCount == 1u)
        {
            // 最後の操作は待機ロック内で 0 にする。Shutdown が 0 を観測した時点では
            // 完了通知側がこのアロケータへ触れ終えていることを保証できる。
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

void FShardedTlsfAllocator::CloseLifecycleGateAndWait() noexcept
{
    m_LifecycleGate.FetchAnd(~kLifecycleAcceptingBit);

    FScopedLock DrainLock(m_LifecycleDrainLock);
    while ((m_LifecycleGate.Load(EMemoryOrder::Acquire) & kLifecycleOperationCountMask) != 0u)
    {
        m_LifecycleDrainedCondition.Wait(m_LifecycleDrainLock);
    }
}

void FShardedTlsfAllocator::OpenLifecycleGate() noexcept
{
    const u64 ClosedGateValue = m_LifecycleGate.Load(EMemoryOrder::Acquire);
    ACS_ASSERT((ClosedGateValue & kLifecycleAcceptingBit) == 0u);
    ACS_ASSERT((ClosedGateValue & kLifecycleOperationCountMask) == 0u);
    ACS_ASSERT(m_Inited && m_ShardCount > 0u);

    u64 NextGeneration = (ClosedGateValue + kLifecycleGenerationIncrement) & kLifecycleGenerationMask;
    if (NextGeneration == 0u)
    {
        NextGeneration = kLifecycleGenerationIncrement;
    }
    m_LifecycleGate.Store(kLifecycleAcceptingBit | NextGeneration, EMemoryOrder::Release);
}

bool FShardedTlsfAllocator::ResetShardsAfterLifecycleClose() noexcept
{
    bool bAllReservationsReleased = true;
    for (u32 ShardIndex = 0u; ShardIndex < m_ShardCount; ++ShardIndex)
    {
        FScopedLock ShardLock(m_Shards[ShardIndex].lock);
        const TResult<void> ResetResult = m_Shards[ShardIndex].alloc.Reset();
        if (ResetResult.IsErr())
        {
            bAllReservationsReleased = false;
        }
    }

    m_Inited = false;
    m_Epoch = 0u;
    m_NextShard.Store(0u, EMemoryOrder::Relaxed);
    if (!bAllReservationsReleased)
    {
        // 解放失敗した予約を次回 Shutdown で再試行できるよう、シャード数を保持する。
        return false;
    }

    m_ShardCount = 0u;
    m_AllocationCount.Store(0u, EMemoryOrder::Release);
    return true;
}

FShardedTlsfAllocator::~FShardedTlsfAllocator() noexcept
{
    Shutdown();
}

TResult<void> FShardedTlsfAllocator::Init(usize TotalReserveBytes, usize CommitInitialBytes,
                                          u32 RequestedShardCount) noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    if (m_Inited || m_ShardCount != 0u) {
        return ACS_ERR(Memory, 50, "FShardedTlsfAllocator: already initialized or shutdown incomplete");
    }

    u32 EffectiveShardCount = RequestedShardCount != 0u ? RequestedShardCount : DetectLogicalCores();
    if (EffectiveShardCount > kMaxShards) EffectiveShardCount = kMaxShards;
    if (EffectiveShardCount == 0u) EffectiveShardCount = 1u;

    const usize AllocationGranularity = VmAllocGranularity();
    const usize PageSize = VmPageSize();

    // 各シャードの予約 / 初期コミットを算出 (粒度整列)。
    usize ReserveBytesPerShard = 0u;
    if (!TryAlignShardSize(TotalReserveBytes / EffectiveShardCount, AllocationGranularity, ReserveBytesPerShard)) {
        return ACS_ERR(Memory, 51, "FShardedTlsfAllocator: reserve size alignment overflow");
    }
    if (ReserveBytesPerShard < 1u * 1024u * 1024u) ReserveBytesPerShard = 1u * 1024u * 1024u;

    usize CommitBytesPerShard = 0u;
    if (!TryAlignShardSize(CommitInitialBytes / EffectiveShardCount, PageSize, CommitBytesPerShard)) {
        return ACS_ERR(Memory, 52, "FShardedTlsfAllocator: commit size alignment overflow");
    }
    const usize kMinCommit = 64u * 1024u;
    if (CommitBytesPerShard < kMinCommit) CommitBytesPerShard = kMinCommit;
    if (CommitBytesPerShard > ReserveBytesPerShard) CommitBytesPerShard = ReserveBytesPerShard;

    for (u32 i = 0; i < EffectiveShardCount; ++i) {
        auto ReservationResult = VmReservation::Reserve(ReserveBytesPerShard);
        if (ReservationResult.IsErr()) {
            (void)ResetShardsAfterLifecycleClose();
            return Err<void>(ReservationResult.Error());
        }
        auto InitResult = m_Shards[i].alloc.InitWithReservation(Move(ReservationResult.Value()), CommitBytesPerShard);
        if (InitResult.IsErr()) {
            (void)ResetShardsAfterLifecycleClose();
            return InitResult;
        }

        // 初期化済み数を都度進める。最後にまとめて設定すると、途中失敗時の
        // Shutdown() が 0 shard と判断し、先に成功した VM 予約を回収できない。
        m_ShardCount = i + 1u;
    }
    m_Epoch = g_epoch_counter.FetchAdd(1) + 1; // 新しい世代 (再 Init でマガジンを無効化)
    m_Inited = true;
    if (m_CacheEnabled.Load(EMemoryOrder::Acquire) != 0u) RegisterThreadCacheLifetime();
    OpenLifecycleGate();
    return Ok();
}

void FShardedTlsfAllocator::EnableThreadCache() noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    m_CacheEnabled.Store(1u, EMemoryOrder::Release);
    if (m_Inited) RegisterThreadCacheLifetime();
}

void FShardedTlsfAllocator::FlushCurrentThreadCache() noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return;
    }
    FlushCurrentThreadCacheWhileLifecycleIsStable();
}

void FShardedTlsfAllocator::FlushCurrentThreadCacheWhileLifecycleIsStable() noexcept
{
    if (t_thread_cache.owner == this)
    {
        ReturnThreadCacheIfLifetimeIsActive(t_thread_cache);
    }
}

void FShardedTlsfAllocator::RegisterThreadCacheLifetime() noexcept
{
    FThreadCacheRegistryLock RegistryLock;
    if (m_ThreadCacheLifetimeRegistered) return;

    m_ThreadCacheRegistryNext = g_thread_cache_registry_head;
    g_thread_cache_registry_head = this;
    m_ThreadCacheLifetimeRegistered = true;
}

void FShardedTlsfAllocator::UnregisterThreadCacheLifetime() noexcept
{
    FThreadCacheRegistryLock RegistryLock;
    if (!m_ThreadCacheLifetimeRegistered) return;

    FShardedTlsfAllocator** Link = &g_thread_cache_registry_head;
    while (*Link && *Link != this) {
        Link = &((*Link)->m_ThreadCacheRegistryNext);
    }
    if (*Link == this) {
        *Link = m_ThreadCacheRegistryNext;
    }

    m_ThreadCacheRegistryNext = nullptr;
    m_ThreadCacheLifetimeRegistered = false;
}

void FShardedTlsfAllocator::ReturnThreadCacheIfLifetimeIsActive(memory_detail::FShardedTlsfThreadCache& Cache) noexcept
{
    if (!Cache.owner) {
        Cache.ClearWithoutReturning();
        return;
    }

    // owner は既に破棄済みかもしれないため、リスト内のポインタと世代が一致するまで参照しない。
    FThreadCacheRegistryLock RegistryLock;
    FShardedTlsfAllocator* Cursor = g_thread_cache_registry_head;
    while (Cursor) {
        if (Cursor == Cache.owner) {
            if (Cursor->m_ThreadCacheLifetimeRegistered && Cursor->m_Inited && Cursor->m_Epoch == Cache.epoch) {
                Cursor->ReturnThreadCacheBlocks(Cache);
            }
            break;
        }
        Cursor = Cursor->m_ThreadCacheRegistryNext;
    }

    // Shutdown/re-init 後の旧世代では VM を触らず、参照だけを失効させる。
    Cache.ClearWithoutReturning();
}

void FShardedTlsfAllocator::ReturnThreadCacheBlocks(memory_detail::FShardedTlsfThreadCache& Cache) noexcept
{
    for (u32 CacheClass = 0; CacheClass < kCacheClasses; ++CacheClass) {
        while (void* const Block = Cache.Pop(CacheClass)) {
            const int ShardIndex = ShardIndexForPtr(Block);
            if (ShardIndex >= 0 && m_Shards[ShardIndex].alloc.TryRestoreThreadCacheBlock(Block)) {
                if (!FreeSharded(Block)) {
                    ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=release_failed "
                                  "operation=return leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                                  Block, CacheClass, static_cast<unsigned long long>(Cache.epoch));
                }
            } else {
                ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                              "operation=return leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                              Block, CacheClass, static_cast<unsigned long long>(Cache.epoch));
            }
        }
    }
}

void FShardedTlsfAllocator::Shutdown() noexcept
{
    FScopedLock LifecycleControlLock(m_LifecycleControlLock);
    CloseLifecycleGateAndWait();

    // 呼び出しスレッド分は VM 解放前に返す。他スレッド分はレジストリ解除後に安全に破棄される。
    FlushCurrentThreadCacheWhileLifecycleIsStable();
    UnregisterThreadCacheLifetime();
    (void)ResetShardsAfterLifecycleClose();
}

int FShardedTlsfAllocator::ShardIndexForThread() noexcept
{
    if (t_assigned_shard < 0) {
        t_assigned_shard = static_cast<int>(m_NextShard.FetchAdd(1));
    }
    return t_assigned_shard % static_cast<int>(m_ShardCount);
}

int FShardedTlsfAllocator::ShardIndexForPtr(const void* Pointer) const noexcept
{
    // ContainsPtr は予約レンジの O(1) 判定 (ロック不要 — 予約 Base/Capacity は Init 後不変)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        if (m_Shards[i].alloc.ContainsPtr(Pointer)) return static_cast<int>(i);
    }
    return -1;
}

void* FShardedTlsfAllocator::AllocSharded(usize Size, usize Alignment, FSourceLoc Location) noexcept
{
    if (!m_Inited || Size == 0) return nullptr;
    const int StartShardIndex = ShardIndexForThread();
    // 自分のシャードから試し、満杯なら隣へフォールバック (偏り/枯渇でも全体予約まで使える)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        const u32 ShardIndex = static_cast<u32>(StartShardIndex + static_cast<int>(i)) % m_ShardCount;
        Shard& CurrentShard = m_Shards[ShardIndex];
        FScopedLock Lock(CurrentShard.lock);
        void* const Result = CurrentShard.alloc.Alloc(Size, Alignment, Location);
        if (Result) return Result;
    }
    return nullptr; // 全シャード満杯 = 真の OOM
}

bool FShardedTlsfAllocator::FreeSharded(void* Pointer) noexcept
{
    if (!Pointer) return true;
    const int ShardIndex = ShardIndexForPtr(Pointer);
    if (ShardIndex < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
        return false;
    }
    FScopedLock Lock(m_Shards[ShardIndex].lock);
    return m_Shards[ShardIndex].alloc.TryFree(Pointer);
}

/**
 * 呼び出しスレッドのマガジンを指定 owner×epoch に整合させて返す。
 *
 * @details
 * 別アロケータを掴んでいたら、生存中の旧 owner へ全ブロックを返してから貼り直す。
 * 旧世代や破棄済み owner の場合は、寿命レジストリ照合により VM に触れず参照だけを捨てる。
 * @param Owner 専有させるアロケータ (通常 this)。
 * @param Epoch そのアロケータの現在世代。
 * @return 整合済みの thread-local マガジンへの参照。
 */
static ACS_FORCEINLINE memory_detail::FShardedTlsfThreadCache& AcquireCache(FShardedTlsfAllocator* Owner,
                                                                            u64 Epoch) noexcept
{
    t_thread_cache.PrepareFor(Owner, Epoch);
    return t_thread_cache;
}

void* FShardedTlsfAllocator::Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return nullptr;
    }
    return AllocateAfterLifecycleAdmission(Size, Alignment, Location);
}

void* FShardedTlsfAllocator::AllocateAfterLifecycleAdmission(usize Size, usize Alignment,
                                                              FSourceLoc Location) noexcept
{
    if (!m_Inited || Size == 0) return nullptr;

    // ---- lock-free hot path: thread-local マガジン ----
    if (m_CacheEnabled.Load(EMemoryOrder::Acquire) != 0u && Alignment <= 16 && Size <= kCacheMaxBlock) {
        const usize CacheBlockSize = CacheBlockSizeForRequest(Size);
        const int CacheClass = CacheClassOfBlock(CacheBlockSize);
        if (CacheClass >= 0) {
            memory_detail::FShardedTlsfThreadCache& ThreadCache = AcquireCache(this, m_Epoch);
            while (void* const Pointer = ThreadCache.Pop(static_cast<u32>(CacheClass))) {
                const int ShardIndex = ShardIndexForPtr(Pointer);
                if (ShardIndex >= 0 && m_Shards[ShardIndex].alloc.TryRestoreThreadCacheBlock(Pointer)) {
                    m_AllocationCount.FetchAdd(1u);
                    return Pointer;
                }
                ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                              "operation=allocate leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                              Pointer, static_cast<u32>(CacheClass), static_cast<unsigned long long>(m_Epoch));
            }
            // ミス: バッチ refill (確保したブロックは実 block_size のクラスへ積む)
            for (u32 i = 0; i < kRefillBatch; ++i) {
                void* const Pointer = AllocSharded(CacheBlockSize - 8u, 16, Location);
                if (!Pointer) break;
                const int RefillClass = CacheClassOfBlock(FTlsfAllocator::PayloadBlockSize(Pointer));
                const int ShardIndex = ShardIndexForPtr(Pointer);
                if (RefillClass >= 0 && ThreadCache.count[RefillClass] < kBucketCap && ShardIndex >= 0 &&
                    m_Shards[ShardIndex].alloc.TryMarkThreadCacheBlock(Pointer)) {
                    if (!ThreadCache.Push(static_cast<u32>(RefillClass), Pointer)) {
                        if (!m_Shards[ShardIndex].alloc.TryRestoreThreadCacheBlock(Pointer)) {
                            ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                                          "operation=refill_push_rollback leak_detected=true pointer=%p "
                                          "cache_class=%u epoch=%llu",
                                          Pointer, static_cast<u32>(RefillClass),
                                          static_cast<unsigned long long>(m_Epoch));
                        } else if (!FreeSharded(Pointer)) {
                            ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=release_failed "
                                          "operation=refill_push_rollback leak_detected=true pointer=%p "
                                          "cache_class=%u epoch=%llu",
                                          Pointer, static_cast<u32>(RefillClass),
                                          static_cast<unsigned long long>(m_Epoch));
                        }
                    }
                } else {
                    // 想定外サイズ等はキャッシュせず返す。
                    if (!FreeSharded(Pointer)) {
                        ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=release_failed "
                                      "operation=refill_discard leak_detected=true pointer=%p cache_class=%u "
                                      "epoch=%llu",
                                      Pointer, RefillClass >= 0 ? static_cast<u32>(RefillClass) : ~u32(0),
                                      static_cast<unsigned long long>(m_Epoch));
                    }
                }
            }
            while (void* const Pointer = ThreadCache.Pop(static_cast<u32>(CacheClass))) {
                const int ShardIndex = ShardIndexForPtr(Pointer);
                if (ShardIndex >= 0 && m_Shards[ShardIndex].alloc.TryRestoreThreadCacheBlock(Pointer)) {
                    m_AllocationCount.FetchAdd(1u);
                    return Pointer;
                }
                ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                              "operation=refill leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                              Pointer, static_cast<u32>(CacheClass), static_cast<unsigned long long>(m_Epoch));
            }
            // refill が要求クラスを満たせなかった (OOM 近傍) → 直接確保
        }
    }
    void* const Result = AllocSharded(Size, Alignment, Location);
    if (Result) m_AllocationCount.FetchAdd(1u);
    return Result;
}

void FShardedTlsfAllocator::Free(void* Pointer) noexcept
{
    if (!Pointer) return;

    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return;
    }
    FreeAfterLifecycleAdmission(Pointer);
}

void FShardedTlsfAllocator::FreeAfterLifecycleAdmission(void* Pointer) noexcept
{
    if (!Pointer) return;

    // ---- thread-local マガジンへ push ----
    // 予約範囲だけでは未コミット領域や payload 内部ポインタも通るため、header を読む前に
    // 所有 shard のロック下で正規ブロック境界と client-owned 状態を検証する。
    if (m_CacheEnabled.Load(EMemoryOrder::Acquire) != 0u && m_Inited) {
        const int ShardIndex = ShardIndexForPtr(Pointer);
        if (ShardIndex < 0) {
            ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
            return;
        }

        int CacheClass = -1;
        bool bFreedDirectly = false;
        {
            FScopedLock Lock(m_Shards[ShardIndex].lock);
            FTlsfAllocator& Allocator = m_Shards[ShardIndex].alloc;
            if (!Allocator.IsClientOwnedBlock(Pointer)) {
                return;
            }

            const usize CurrentBlockSize = FTlsfAllocator::PayloadBlockSize(Pointer);
            CacheClass = CacheClassOfBlock(CurrentBlockSize);
            if (CacheClass >= 0) {
                if (!Allocator.TryMarkThreadCacheBlock(Pointer)) {
                    return;
                }
            } else {
                bFreedDirectly = Allocator.TryFree(Pointer);
            }
        }

        if (CacheClass >= 0) {
            memory_detail::FShardedTlsfThreadCache& ThreadCache = AcquireCache(this, m_Epoch);
            if (ThreadCache.count[CacheClass] >= kBucketCap) {
                // バケット満杯: 半分を実シャードへ返してから push (償却)
                for (u32 i = 0; i < kBucketCap / 2; ++i) {
                    void* const CachedPointer = ThreadCache.Pop(static_cast<u32>(CacheClass));
                    if (!CachedPointer) break;
                    const int CachedShardIndex = ShardIndexForPtr(CachedPointer);
                    if (CachedShardIndex >= 0 &&
                        m_Shards[CachedShardIndex].alloc.TryRestoreThreadCacheBlock(CachedPointer)) {
                        if (!FreeSharded(CachedPointer)) {
                            ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=release_failed "
                                          "operation=evict leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                                          CachedPointer, static_cast<u32>(CacheClass),
                                          static_cast<unsigned long long>(m_Epoch));
                        }
                    } else {
                        ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                                      "operation=evict leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                                      CachedPointer, static_cast<u32>(CacheClass),
                                      static_cast<unsigned long long>(m_Epoch));
                    }
                }
            }
            if (!ThreadCache.Push(static_cast<u32>(CacheClass), Pointer)) {
                const int RollbackShardIndex = ShardIndexForPtr(Pointer);
                if (RollbackShardIndex >= 0 && m_Shards[RollbackShardIndex].alloc.TryRestoreThreadCacheBlock(Pointer)) {
                    if (!FreeSharded(Pointer)) {
                        ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=release_failed "
                                      "operation=push_rollback leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                                      Pointer, static_cast<u32>(CacheClass), static_cast<unsigned long long>(m_Epoch));
                    }
                } else {
                    ACS_LOG_ERROR("[acs][memory] tracker=sharded_tlsf_thread_cache record=restore_failed "
                                  "operation=push_rollback leak_detected=true pointer=%p cache_class=%u epoch=%llu",
                                  Pointer, static_cast<u32>(CacheClass), static_cast<unsigned long long>(m_Epoch));
                }
            }
            m_AllocationCount.FetchSub(1u);
            return;
        }

        // キャッシュ対象外サイズは上の所有検証と同じロック区間で直接返している。
        if (bFreedDirectly) {
            m_AllocationCount.FetchSub(1u);
        }
        return;
    }
    const int ShardIndex = ShardIndexForPtr(Pointer);
    if (ShardIndex < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
        return;
    }
    if (FreeSharded(Pointer)) {
        m_AllocationCount.FetchSub(1u);
    }
}

void* FShardedTlsfAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                                     FSourceLoc Location) noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return Pointer && NewSize == 0u ? Pointer : nullptr;
    }
    return ReallocateAfterLifecycleAdmission(Pointer, OldSize, NewSize, Alignment, Location);
}

void* FShardedTlsfAllocator::ReallocateAfterLifecycleAdmission(void* Pointer, usize OldSize, usize NewSize,
                                                                usize Alignment, FSourceLoc Location) noexcept
{
    if (!m_Inited) return Pointer && NewSize == 0u ? Pointer : nullptr;
    if (Pointer == nullptr) return AllocateAfterLifecycleAdmission(NewSize, Alignment, Location);

    const int ShardIndex = ShardIndexForPtr(Pointer);
    if (ShardIndex < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Realloc: 所有シャード不明");
        return NewSize == 0u ? Pointer : nullptr;
    }
    {
        // new_size==0 も所有権を確認してから解放する。不正ポインタなら元の値を返して保持を伝える。
        FScopedLock Lock(m_Shards[ShardIndex].lock);
        if (!m_Shards[ShardIndex].alloc.IsClientOwnedBlock(Pointer)) {
            return NewSize == 0u ? Pointer : nullptr;
        }
        if (NewSize == 0u) {
            if (!m_Shards[ShardIndex].alloc.TryFree(Pointer)) {
                return Pointer;
            }
            m_AllocationCount.FetchSub(1u);
            return nullptr;
        }

        // 同一シャード内で in-place / 同シャード移動を試みる。
        void* const Result = m_Shards[ShardIndex].alloc.Realloc(Pointer, OldSize, NewSize, Alignment, Location);
        if (Result) return Result;
    }
    // 同シャードで不可 → 別シャードへ移動。先に新規確保を終えてから旧シャードだけを
    // 再ロックするため、シャードロックは重ならない。並行 Free とコピーを競合させないよう、
    // 所有状態の再確認・実 payload 容量までのコピー・旧解放を同じロック区間で行う。
    void* const NewPointer = AllocateAfterLifecycleAdmission(NewSize, Alignment, Location);
    if (!NewPointer) return nullptr;

    bool bOldAllocationReleased = false;
    {
        FScopedLock Lock(m_Shards[ShardIndex].lock);
        FTlsfAllocator& OldAllocator = m_Shards[ShardIndex].alloc;
        if (OldAllocator.IsClientOwnedBlock(Pointer)) {
            usize CopySize = OldSize < NewSize ? OldSize : NewSize;
            const usize PayloadCapacity = FTlsfAllocator::PayloadCapacity(Pointer);
            if (CopySize > PayloadCapacity) CopySize = PayloadCapacity;
            MemCopy(NewPointer, Pointer, CopySize);
            bOldAllocationReleased = OldAllocator.TryFree(Pointer);
        }
    }

    if (!bOldAllocationReleased) {
        // 競合解放または所有状態の破損を検出した場合、新規領域を返して二重所有にしない。
        FreeAfterLifecycleAdmission(NewPointer);
        return nullptr;
    }
    m_AllocationCount.FetchSub(1u);
    return NewPointer;
}

u64 FShardedTlsfAllocator::BytesAllocated() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return 0u;
    }

    u64 TotalBytes = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock Lock(m_Shards[i].lock);
        TotalBytes += m_Shards[i].alloc.BytesAllocated();
    }
    return TotalBytes;
}

u64 FShardedTlsfAllocator::AllocationCount() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return 0u;
    }
    return m_AllocationCount.Load(EMemoryOrder::Acquire);
}

u64 FShardedTlsfAllocator::PeakBytes() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return 0u;
    }

    // 各シャードのピーク合算は厳密な「同時ピーク」ではないが、上限の目安として有用。
    u64 TotalBytes = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock Lock(m_Shards[i].lock);
        TotalBytes += m_Shards[i].alloc.PeakBytes();
    }
    return TotalBytes;
}

u32 FShardedTlsfAllocator::ShardCount() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return 0u;
    }
    return m_ShardCount;
}

bool FShardedTlsfAllocator::ThreadCacheEnabled() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return false;
    }
    return m_CacheEnabled.Load(EMemoryOrder::Acquire) != 0u;
}

u64 FShardedTlsfAllocator::Epoch() const noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return 0u;
    }
    return m_Epoch;
}

bool FShardedTlsfAllocator::ValidateHeap() noexcept
{
    FLifecycleOperation LifecycleOperation(*this);
    if (!LifecycleOperation.WasAdmitted())
    {
        return false;
    }

    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock Lock(m_Shards[i].lock);
        if (!m_Shards[i].alloc.ValidateHeap()) return false;
    }
    return true;
}

} // namespace acs
