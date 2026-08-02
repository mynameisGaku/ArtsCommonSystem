// SPDX-License-Identifier: Apache-2.0
// backing の寿命世代不一致を隔離プロセスで検証する。
#include "MemoryAllocator.h"
#include "foundation/Platform.h"
#include "memory/Allocator.h"
#include "render/Diligent/DiligentMemoryAdapter.h"

using namespace acs;

namespace {

/** 寿命世代と VM 保護をテスト側から制御できる allocator。 */
class FLifetimeTestAllocator final : public IAllocator {
public:
    explicit FLifetimeTestAllocator(u64 lifetime_generation) noexcept : m_LifetimeGeneration(lifetime_generation)
    {
    }

    ~FLifetimeTestAllocator() noexcept override
    {
        if (m_Allocation) {
            (void)::VirtualFree(m_Allocation, 0, MEM_RELEASE);
            m_Allocation = nullptr;
        }
    }

    void* Alloc(usize size, usize alignment, FSourceLoc) noexcept override
    {
        if (size == 0 || m_Allocation || alignment == 0 || alignment > 64u * 1024u) return nullptr;
        m_Allocation = ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        m_AllocationSize = m_Allocation ? size : 0;
        return m_Allocation;
    }

    void Free(void* pointer) noexcept override
    {
        ++m_FreeCallCount;
        if (m_ChangeGenerationDuringFree) {
            m_LifetimeGeneration = 0;
            m_ChangeGenerationDuringFree = false;
            return;
        }

        if (pointer == m_Allocation && m_Allocation) {
            (void)::VirtualFree(m_Allocation, 0, MEM_RELEASE);
            m_Allocation = nullptr;
            m_AllocationSize = 0;
            ++m_CompletedFreeCount;
        }
    }

    u64 LifetimeGeneration() const noexcept override
    {
        return m_LifetimeGeneration;
    }

    void SetLifetimeGeneration(u64 lifetime_generation) noexcept
    {
        m_LifetimeGeneration = lifetime_generation;
    }

    void ChangeGenerationDuringNextFree() noexcept
    {
        m_ChangeGenerationDuringFree = true;
    }

    bool ProtectAllocation(u32 protection) noexcept
    {
        if (!m_Allocation || m_AllocationSize == 0) return false;
        DWORD previous_protection = 0;
        return ::VirtualProtect(m_Allocation, m_AllocationSize, static_cast<DWORD>(protection), &previous_protection) !=
               FALSE;
    }

    u32 FreeCallCount() const noexcept
    {
        return m_FreeCallCount;
    }

    u32 CompletedFreeCount() const noexcept
    {
        return m_CompletedFreeCount;
    }

private:
    void* m_Allocation = nullptr;
    usize m_AllocationSize = 0;
    u64 m_LifetimeGeneration = 0;
    u32 m_FreeCallCount = 0;
    u32 m_CompletedFreeCount = 0;
    bool m_ChangeGenerationDuringFree = false;
};

bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return left == right;
    while (*left && *right) {
        if (*left != *right) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

int RunStaleLifetimeTest()
{
    FLifetimeTestAllocator allocator(1u);
    void* const raw_adapter = CDiligentMemoryAdapter::Create(&allocator);
    if (!raw_adapter) return 81;

    auto* const adapter = static_cast<Diligent::IMemoryAllocator*>(raw_adapter);
    void* const pointer = adapter->Allocate(4096u, "stale-lifetime-test", __FILE__, __LINE__);
    if (!pointer) return 82;
    if (CDiligentMemoryAdapter::OutstandingAllocationCount() != 1u) return 83;
    if (!allocator.ProtectAllocation(PAGE_NOACCESS)) return 84;

    // 利用者領域を読まず、レジストリと backing 世代だけで拒否しなければならない。
    allocator.SetLifetimeGeneration(0);
    adapter->Free(pointer);
    if (allocator.FreeCallCount() != 0u || CDiligentMemoryAdapter::OutstandingAllocationCount() != 1u ||
        CDiligentMemoryAdapter::OutstandingRequestedBytes() != 4096u) {
        return 85;
    }

    // 世代を復元して同じ記録を正常解放し、静的アダプタを汚染しない。
    allocator.SetLifetimeGeneration(1u);
    adapter->Free(pointer);
    if (allocator.FreeCallCount() != 1u || allocator.CompletedFreeCount() != 1u ||
        CDiligentMemoryAdapter::OutstandingAllocationCount() != 0u ||
        CDiligentMemoryAdapter::OutstandingRequestedBytes() != 0u) {
        return 86;
    }
    return 0;
}

int RunGenerationChangeDuringFreeTest()
{
    FLifetimeTestAllocator allocator(1u);
    void* const raw_adapter = CDiligentMemoryAdapter::Create(&allocator);
    if (!raw_adapter) return 91;

    auto* const adapter = static_cast<Diligent::IMemoryAllocator*>(raw_adapter);
    void* const pointer = adapter->Allocate(8192u, "generation-change-during-free", __FILE__, __LINE__);
    if (!pointer) return 92;

    // backing Free 中に寿命世代が変わった場合、解放記録と件数を確定してはならない。
    allocator.ChangeGenerationDuringNextFree();
    adapter->Free(pointer);
    if (allocator.FreeCallCount() != 1u || allocator.CompletedFreeCount() != 0u ||
        CDiligentMemoryAdapter::OutstandingAllocationCount() != 1u ||
        CDiligentMemoryAdapter::OutstandingRequestedBytes() != 8192u) {
        return 93;
    }

    allocator.SetLifetimeGeneration(1u);
    adapter->Free(pointer);
    if (allocator.FreeCallCount() != 2u || allocator.CompletedFreeCount() != 1u ||
        CDiligentMemoryAdapter::OutstandingAllocationCount() != 0u ||
        CDiligentMemoryAdapter::OutstandingRequestedBytes() != 0u) {
        return 94;
    }
    return 0;
}

} // namespace

int main(int argument_count, char** arguments)
{
    if (argument_count == 1) return RunStaleLifetimeTest();
    if (argument_count == 2 && TextEquals(arguments[1], "--generation-change-during-free")) {
        return RunGenerationChangeDuringFreeTest();
    }
    return 99;
}
