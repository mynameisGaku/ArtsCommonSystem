// SPDX-License-Identifier: Apache-2.0
// CDiligentMemoryAdapter 実装
//
// Diligent の IMemoryAllocator は以下の仮想関数を持つ:
//   void* Allocate(size_t Size, const Char* dbgInfo, const Char* dbgFile, const Int32 dbgLine);
//   void  Free(void* Ptr);
// IObject は継承しない（軽量インターフェイス）。
#include "render/Diligent/DiligentMemoryAdapter.h"

#if WITH_RENDER_DILIGENT

#    include "render/Diligent/DiligentCommon.h"
#    include "MemoryAllocator.h" // Diligent/Common
#    include "memory/Allocator.h"
#    include "foundation/Log.h"
#    include "foundation/Platform.h"
#    include "foundation/SourceLoc.h"
#    include "foundation/Assert.h"
#    include "threading/RwLock.h"
#    include "threading/ScopedLock.h"
#    include "threading/Atomic.h"

#    include <cstddef> // max_align_t

namespace acs {

namespace {

enum class EAllocationRegistryLookup : u8 {
    Ready,
    NotFound,
    LifetimeMismatch,
    FreeInProgress,
};

/** backing 領域を読まずに Free の所有権と寿命を検証するプロセス寿命レコード。 */
struct FAllocationRegistryRecord {
    void* pointer = nullptr;
    void* raw = nullptr;
    IAllocator* allocator = nullptr;
    u64 allocator_lifetime_generation = 0;
    u64 adapter_binding_generation = 0;
    u64 requested_bytes = 0;
    u8 state = 0;
};

/** STL コンテナへ依存せず、Win32 process heap 上で伸長する open-addressing レジストリ。 */
class FAllocationRegistry final {
public:
    ~FAllocationRegistry() noexcept
    {
        if (m_Records) {
            (void)::HeapFree(::GetProcessHeap(), 0, m_Records);
            m_Records = nullptr;
        }
    }

    FAllocationRegistry() noexcept = default;
    FAllocationRegistry(const FAllocationRegistry&) = delete;
    FAllocationRegistry& operator=(const FAllocationRegistry&) = delete;

    bool Insert(const FAllocationRegistryRecord& record) noexcept
    {
        FScopedExclusiveLock lock(m_Lock);
        if (!record.pointer || !record.raw || !record.allocator || record.allocator_lifetime_generation == 0 ||
            record.adapter_binding_generation == 0 || record.requested_bytes == 0) {
            return false;
        }

        if (!EnsureInsertCapacity()) return false;
        return InsertInto(m_Records, m_Capacity, record, m_Count, m_TombstoneCount);
    }

    EAllocationRegistryLookup BeginFree(void* pointer, u64 expected_binding_generation,
                                        FAllocationRegistryRecord& output,
                                        u64& current_allocator_lifetime_generation) noexcept
    {
        FScopedExclusiveLock lock(m_Lock);
        current_allocator_lifetime_generation = 0;
        const usize index = FindIndex(pointer);
        if (index == kInvalidIndex) return EAllocationRegistryLookup::NotFound;

        FAllocationRegistryRecord& record = m_Records[index];
        output = record;
        if (record.state == kFreeingState) return EAllocationRegistryLookup::FreeInProgress;
        if (record.allocator) {
            current_allocator_lifetime_generation = record.allocator->LifetimeGeneration();
        }
        if (record.adapter_binding_generation != expected_binding_generation ||
            current_allocator_lifetime_generation == 0 ||
            current_allocator_lifetime_generation != record.allocator_lifetime_generation) {
            return EAllocationRegistryLookup::LifetimeMismatch;
        }

        record.state = kFreeingState;
        return EAllocationRegistryLookup::Ready;
    }

    bool CompleteFree(const FAllocationRegistryRecord& expected, bool backing_free_confirmed) noexcept
    {
        FScopedExclusiveLock lock(m_Lock);
        const usize index = FindIndex(expected.pointer);
        if (index == kInvalidIndex) return false;

        FAllocationRegistryRecord& record = m_Records[index];
        if (record.state != kFreeingState || record.pointer != expected.pointer || record.raw != expected.raw ||
            record.allocator != expected.allocator ||
            record.allocator_lifetime_generation != expected.allocator_lifetime_generation ||
            record.adapter_binding_generation != expected.adapter_binding_generation ||
            record.requested_bytes != expected.requested_bytes) {
            return false;
        }

        if (!backing_free_confirmed) {
            record.state = kOccupiedState;
            return false;
        }

        record = {};
        record.state = kTombstoneState;
        --m_Count;
        ++m_TombstoneCount;
        return true;
    }

private:
    static constexpr u8 kEmptyState = 0;
    static constexpr u8 kOccupiedState = 1;
    static constexpr u8 kTombstoneState = 2;
    static constexpr u8 kFreeingState = 3;
    static constexpr usize kInitialCapacity = 1024;
    static constexpr usize kInvalidIndex = ~usize{0};

    static usize HashPointer(const void* pointer) noexcept
    {
        uptr value = reinterpret_cast<uptr>(pointer) >> 4u;
        value ^= value >> 17u;
        value *= static_cast<uptr>(0x9E3779B1u);
        value ^= value >> 11u;
        return static_cast<usize>(value);
    }

    static FAllocationRegistryRecord* AllocateRecords(usize capacity) noexcept
    {
        if (capacity == 0 || capacity > (~usize{0}) / sizeof(FAllocationRegistryRecord)) return nullptr;
        return static_cast<FAllocationRegistryRecord*>(
            ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(FAllocationRegistryRecord)));
    }

    static bool InsertInto(FAllocationRegistryRecord* records, usize capacity, const FAllocationRegistryRecord& input,
                           usize& count, usize& tombstone_count) noexcept
    {
        const usize mask = capacity - 1u;
        const u8 destination_state = input.state == kFreeingState ? kFreeingState : kOccupiedState;
        usize first_tombstone = kInvalidIndex;
        for (usize probe = 0; probe < capacity; ++probe) {
            const usize index = (HashPointer(input.pointer) + probe) & mask;
            FAllocationRegistryRecord& record = records[index];
            if (record.state == kOccupiedState || record.state == kFreeingState) {
                if (record.pointer == input.pointer) return false;
                continue;
            }
            if (record.state == kTombstoneState) {
                if (first_tombstone == kInvalidIndex) first_tombstone = index;
                continue;
            }

            const usize destination = first_tombstone != kInvalidIndex ? first_tombstone : index;
            records[destination] = input;
            records[destination].state = destination_state;
            ++count;
            if (first_tombstone != kInvalidIndex) --tombstone_count;
            return true;
        }

        if (first_tombstone != kInvalidIndex) {
            records[first_tombstone] = input;
            records[first_tombstone].state = destination_state;
            ++count;
            --tombstone_count;
            return true;
        }
        return false;
    }

    bool EnsureInsertCapacity() noexcept
    {
        if (!m_Records) return Rebuild(kInitialCapacity);

        const usize unavailable_count = m_Count + m_TombstoneCount;
        if (unavailable_count + 1u < m_Capacity - (m_Capacity / 4u)) return true;

        usize next_capacity = m_Capacity;
        if (m_Count + 1u >= m_Capacity / 2u) {
            if (m_Capacity > (~usize{0}) / 2u) return false;
            next_capacity = m_Capacity * 2u;
        }
        return Rebuild(next_capacity);
    }

    bool Rebuild(usize new_capacity) noexcept
    {
        FAllocationRegistryRecord* const new_records = AllocateRecords(new_capacity);
        if (!new_records) return false;

        usize new_count = 0;
        usize new_tombstone_count = 0;
        for (usize index = 0; index < m_Capacity; ++index) {
            if ((m_Records[index].state == kOccupiedState || m_Records[index].state == kFreeingState) &&
                !InsertInto(new_records, new_capacity, m_Records[index], new_count, new_tombstone_count)) {
                (void)::HeapFree(::GetProcessHeap(), 0, new_records);
                return false;
            }
        }

        if (m_Records) (void)::HeapFree(::GetProcessHeap(), 0, m_Records);
        m_Records = new_records;
        m_Capacity = new_capacity;
        m_Count = new_count;
        m_TombstoneCount = 0;
        return true;
    }

    usize FindIndex(void* pointer) const noexcept
    {
        if (!pointer || !m_Records || m_Capacity == 0) return kInvalidIndex;
        const usize mask = m_Capacity - 1u;
        for (usize probe = 0; probe < m_Capacity; ++probe) {
            const usize index = (HashPointer(pointer) + probe) & mask;
            const FAllocationRegistryRecord& record = m_Records[index];
            if (record.state == kEmptyState) return kInvalidIndex;
            if ((record.state == kOccupiedState || record.state == kFreeingState) && record.pointer == pointer) {
                return index;
            }
        }
        return kInvalidIndex;
    }

    FRwLock m_Lock;
    FAllocationRegistryRecord* m_Records = nullptr;
    usize m_Capacity = 0;
    usize m_Count = 0;
    usize m_TombstoneCount = 0;
};

/**
 * ACS の IAllocator へ委譲する Diligent::IMemoryAllocator 実装。
 *
 * @details
 * Diligent の確保・解放要求を backing IAllocator へ流す。アライメントは max_align_t に
 * 固定し、確保ごとに確保元を記録するため、アダプタの再バインド後も正しい元へ解放できる。
 */
class FAcsMemoryAllocator final : public Diligent::IMemoryAllocator {
public:
    /** 委譲先未設定の静的アダプタを構築する。 */
    FAcsMemoryAllocator() noexcept = default;

    /**
     * 以後の確保に使う委譲先アロケータを設定する。
     *
     * @param backing 実際の確保・解放を委譲する ACS アロケータ。
     */
    bool Bind(IAllocator* backing) noexcept
    {
        FScopedExclusiveLock lock(m_LifetimeLock);
        if (!backing) return false;
        const u64 backing_lifetime_generation = backing->LifetimeGeneration();
        if (backing_lifetime_generation == 0) return false;
        if (m_OutstandingAllocationCount.Load(EMemoryOrder::Acquire) != 0) return false;
        m_Backing = backing;
        m_BackingLifetimeGeneration = backing_lifetime_generation;
        ++m_BindingGeneration;
        if (m_BindingGeneration == 0) ++m_BindingGeneration;
        return true;
    }

    /**
     * backing 経由でメモリを確保する。
     *
     * @details backing が null または Size が 0 のときは nullptr を返す。デバッグ引数は
     * FSourceLoc へ変換し、backing の確保診断へ引き渡す。
     * @param Size 確保するバイト数。
     * @param dbgInfo Diligent のデバッグ用説明文字列。
     * @param dbgFile Diligent のデバッグ用ソースファイル名。
     * @param dbgLine Diligent のデバッグ用行番号。
     * @return 確保したメモリ (失敗時は nullptr)。
     */
    void* Allocate(size_t Size, const Diligent::Char* dbgInfo, const Diligent::Char* dbgFile,
                   const Diligent::Int32 dbgLine) override
    {
        if (Size == 0) return nullptr;

        FScopedSharedLock lock(m_LifetimeLock);

        IAllocator* const allocator = m_Backing;
        if (!allocator) return nullptr;
        const u64 allocator_lifetime_generation = allocator->LifetimeGeneration();
        if (allocator_lifetime_generation == 0 || allocator_lifetime_generation != m_BackingLifetimeGeneration) {
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=allocation_rejected "
                          "reason=backing_lifetime_mismatch binding_generation=%llu "
                          "expected_backing_lifetime_generation=%llu current_backing_lifetime_generation=%llu",
                          static_cast<unsigned long long>(m_BindingGeneration),
                          static_cast<unsigned long long>(m_BackingLifetimeGeneration),
                          static_cast<unsigned long long>(allocator_lifetime_generation));
            return nullptr;
        }

        constexpr usize alignment = alignof(::max_align_t);
        const u32 source_line = dbgLine > 0 ? static_cast<u32>(dbgLine) : 0u;
        void* const raw = allocator->Alloc(static_cast<usize>(Size), alignment,
                                           FSourceLoc::Create(dbgFile, dbgInfo, source_line));
        if (!raw) return nullptr;

        FAllocationRegistryRecord record{};
        record.pointer = raw;
        record.raw = raw;
        record.allocator = allocator;
        record.allocator_lifetime_generation = allocator_lifetime_generation;
        record.adapter_binding_generation = m_BindingGeneration;
        record.requested_bytes = static_cast<u64>(Size);
        if (!m_AllocationRegistry.Insert(record)) {
            if (allocator->LifetimeGeneration() == allocator_lifetime_generation) allocator->Free(raw);
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=allocation_rejected "
                          "reason=registry_insert_failed binding_generation=%llu "
                          "backing_lifetime_generation=%llu requested_bytes=%llu",
                          static_cast<unsigned long long>(m_BindingGeneration),
                          static_cast<unsigned long long>(allocator_lifetime_generation),
                          static_cast<unsigned long long>(Size));
            return nullptr;
        }

        m_OutstandingAllocationCount.FetchAdd(1);
        m_OutstandingRequestedBytes.FetchAdd(static_cast<u64>(Size));
        return raw;
    }

    /**
     * backing 経由でメモリを解放する。
     *
     * @param Ptr 解放するメモリ (null なら何もしない)。
     */
    void Free(void* Ptr) override
    {
        if (!Ptr) return;
        // backing Free 直後の同一アドレス再割り当てを、レジストリ確定まで待たせる。
        FScopedExclusiveLock lock(m_LifetimeLock);
        FAllocationRegistryRecord record{};
        u64 current_allocator_lifetime_generation = 0;
        const EAllocationRegistryLookup lookup = m_AllocationRegistry.BeginFree(Ptr, m_BindingGeneration, record,
                                                                                current_allocator_lifetime_generation);
        if (lookup == EAllocationRegistryLookup::NotFound) {
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=invalid_free "
                          "reason=untracked_pointer pointer=%p binding_generation=%llu",
                          Ptr, static_cast<unsigned long long>(m_BindingGeneration));
            return;
        }
        if (lookup == EAllocationRegistryLookup::LifetimeMismatch) {
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=stale_free_rejected "
                          "leak_detected=true pointer=%p current_binding_generation=%llu "
                          "allocation_binding_generation=%llu allocation_backing_lifetime_generation=%llu "
                          "current_backing_lifetime_generation=%llu requested_bytes=%llu",
                          Ptr, static_cast<unsigned long long>(m_BindingGeneration),
                          static_cast<unsigned long long>(record.adapter_binding_generation),
                          static_cast<unsigned long long>(record.allocator_lifetime_generation),
                          static_cast<unsigned long long>(current_allocator_lifetime_generation),
                          static_cast<unsigned long long>(record.requested_bytes));
            return;
        }
        if (lookup == EAllocationRegistryLookup::FreeInProgress) {
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=invalid_free "
                          "reason=free_in_progress pointer=%p binding_generation=%llu requested_bytes=%llu",
                          Ptr, static_cast<unsigned long long>(m_BindingGeneration),
                          static_cast<unsigned long long>(record.requested_bytes));
            return;
        }

        record.allocator->Free(record.raw);
        const u64 post_free_allocator_lifetime_generation = record.allocator->LifetimeGeneration();
        const bool backing_free_confirmed = post_free_allocator_lifetime_generation != 0 &&
                                            post_free_allocator_lifetime_generation ==
                                                record.allocator_lifetime_generation;
        if (!m_AllocationRegistry.CompleteFree(record, backing_free_confirmed)) {
            ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=free_completion_unconfirmed "
                          "leak_detected=true pointer=%p binding_generation=%llu "
                          "expected_backing_lifetime_generation=%llu post_free_backing_lifetime_generation=%llu "
                          "requested_bytes=%llu",
                          Ptr, static_cast<unsigned long long>(m_BindingGeneration),
                          static_cast<unsigned long long>(record.allocator_lifetime_generation),
                          static_cast<unsigned long long>(post_free_allocator_lifetime_generation),
                          static_cast<unsigned long long>(record.requested_bytes));
            return;
        }

        const u64 previous = m_OutstandingAllocationCount.FetchSub(1);
        const u64 previous_bytes = m_OutstandingRequestedBytes.FetchSub(record.requested_bytes);
        ACS_ASSERT(previous > 0);
        ACS_ASSERT(previous_bytes >= record.requested_bytes);
    }

    /** 現在の未解放件数を返す。 */
    u64 OutstandingAllocationCount() const noexcept
    {
        return m_OutstandingAllocationCount.Load(EMemoryOrder::Acquire);
    }

    /** 現在の未解放要求バイト数を返す。 */
    u64 OutstandingRequestedBytes() const noexcept
    {
        return m_OutstandingRequestedBytes.Load(EMemoryOrder::Acquire);
    }

    /** 現在の成功バインド世代を返す。 */
    u64 BindingGeneration() noexcept
    {
        FScopedSharedLock lock(m_LifetimeLock);
        return m_BindingGeneration;
    }

    /** 現在バインドしている backing allocator の寿命世代を返す。 */
    u64 BackingLifetimeGeneration() noexcept
    {
        FScopedSharedLock lock(m_LifetimeLock);
        return m_BackingLifetimeGeneration;
    }

private:
    /** backing の差し替えを確保・解放に対して直列化し、解放トランザクションを排他化する寿命ロック。 */
    FRwLock m_LifetimeLock;

    /** backing 領域から独立し、旧ポインタを参照せず所有権を判定するレジストリ。 */
    FAllocationRegistry m_AllocationRegistry;

    /** 確保・解放を委譲する ACS アロケータ。 */
    IAllocator* m_Backing = nullptr;

    /** Create 成功時に固定した backing allocator の寿命世代。 */
    u64 m_BackingLifetimeGeneration = 0;

    /** 成功した Create ごとに進み、レジストリの確保記録と照合する寿命世代。 */
    u64 m_BindingGeneration = 0;

    /** backing が生存していなければならない未解放領域数。 */
    TAtomic<u64> m_OutstandingAllocationCount{0};

    /** backing が生存していなければならない未解放領域の要求サイズ合計。 */
    TAtomic<u64> m_OutstandingRequestedBytes{0};
};

/** 静的寿命のアダプタ実体を一箇所から取得する。 */
FAcsMemoryAllocator& AdapterInstance() noexcept
{
    static FAcsMemoryAllocator adapter;
    return adapter;
}

} // namespace

/** backing を委譲先にする静的寿命のアダプタを返す。 */
void* CDiligentMemoryAdapter::Create(IAllocator* backing) noexcept
{
    if (!backing) return nullptr;
    // アダプタ本体は backing の外に置く。同一アドレスが再利用されても、旧寿命の領域が残る間は
    // 再バインドを拒否し、解放済み VM を新しい allocator の領域として扱わない。
    FAcsMemoryAllocator& adapter = AdapterInstance();
    if (!adapter.Bind(backing)) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=bind_rejected "
                      "backing=%p requested_backing_lifetime_generation=%llu "
                      "current_backing_lifetime_generation=%llu binding_generation=%llu outstanding_allocations=%llu "
                      "outstanding_requested_bytes=%llu",
                      static_cast<void*>(backing), static_cast<unsigned long long>(backing->LifetimeGeneration()),
                      static_cast<unsigned long long>(adapter.BackingLifetimeGeneration()),
                      static_cast<unsigned long long>(adapter.BindingGeneration()),
                      static_cast<unsigned long long>(adapter.OutstandingAllocationCount()),
                      static_cast<unsigned long long>(adapter.OutstandingRequestedBytes()));
        return nullptr;
    }
    return static_cast<void*>(&adapter);
}

u64 CDiligentMemoryAdapter::OutstandingAllocationCount() noexcept
{
    return AdapterInstance().OutstandingAllocationCount();
}

u64 CDiligentMemoryAdapter::OutstandingRequestedBytes() noexcept
{
    return AdapterInstance().OutstandingRequestedBytes();
}

u64 CDiligentMemoryAdapter::BindingGeneration() noexcept
{
    return AdapterInstance().BindingGeneration();
}

u64 CDiligentMemoryAdapter::BackingLifetimeGeneration() noexcept
{
    return AdapterInstance().BackingLifetimeGeneration();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
