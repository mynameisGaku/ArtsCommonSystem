// SPDX-License-Identifier: Apache-2.0
// 長寿命オブジェクトのアロケータ所有権契約テスト。
#include "test/Test.h"
#include "test/Expect.h"

#include "easy/Easy.h"
#include "foundation/Move.h"
#include "gameframework/DevConsole.h"
#if WITH_RENDER_DX12_RAW
#    include "gameframework/tools/editor_core/EditorCommand.h"
#    include "gameframework/tools/editor_core/UndoStack.h"
#endif
#include "memory/Memory.h"
#include "memory/ObjectPool.h"
#include "memory/SystemAllocator.h"
#include "memory/UniquePtr.h"
#include "render/Particles.h"

using namespace acs;
using namespace acs::game;

namespace {

class FOwnershipAllocator final : public IAllocator {
public:
    struct FHeader {
        FOwnershipAllocator* owner;
        void* raw;
        usize size;
    };

    void* Alloc(usize Size, usize Alignment, FSourceLoc) noexcept override
    {
        if (fail_allocations || Size == 0) return nullptr;
        usize EffectiveAlignment = Alignment;
        if (EffectiveAlignment < alignof(FHeader)) EffectiveAlignment = alignof(FHeader);
        if (EffectiveAlignment == 0 || (EffectiveAlignment & (EffectiveAlignment - 1)) != 0) {
            return nullptr;
        }
        if (Size > (~usize(0)) - sizeof(FHeader) - (EffectiveAlignment - 1)) return nullptr;

        const usize TotalSize = Size + sizeof(FHeader) + EffectiveAlignment - 1;
        void* const RawAllocation = backing.Alloc(TotalSize, EffectiveAlignment, FSourceLoc::Current());
        if (RawAllocation == nullptr) return nullptr;

        const usize AllocationBegin = reinterpret_cast<usize>(RawAllocation) + sizeof(FHeader);
        const usize AlignedAddress = (AllocationBegin + EffectiveAlignment - 1) & ~(EffectiveAlignment - 1);
        auto* const AllocationHeader = reinterpret_cast<FHeader*>(AlignedAddress - sizeof(FHeader));
        AllocationHeader->owner = this;
        AllocationHeader->raw = RawAllocation;
        AllocationHeader->size = Size;
        outstanding_bytes += Size;
        ++outstanding_allocations;
        return reinterpret_cast<void*>(AlignedAddress);
    }

    void Free(void* Pointer) noexcept override
    {
        if (Pointer == nullptr) return;
        auto* const AllocationHeader = reinterpret_cast<FHeader*>(reinterpret_cast<usize>(Pointer) - sizeof(FHeader));
        FOwnershipAllocator* const Owner = AllocationHeader->owner;
        if (Owner != this) ++foreign_free_count;
        if (Owner == nullptr) return;
        Owner->outstanding_bytes -= AllocationHeader->size;
        --Owner->outstanding_allocations;
        Owner->backing.Free(AllocationHeader->raw);
    }

    u64 BytesAllocated() const noexcept override
    {
        return outstanding_bytes;
    }
    const char* Name() const noexcept override
    {
        return "OwnershipTest";
    }

    CSystemAllocator backing;
    u64 outstanding_bytes = 0;
    u32 outstanding_allocations = 0;
    u32 foreign_free_count = 0;
    bool fail_allocations = false;
};

class FDefaultAllocatorScope {
public:
    explicit FDefaultAllocatorScope(IAllocator& Allocator) noexcept : previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&Allocator);
    }

    ~FDefaultAllocatorScope() noexcept
    {
        SetDefaultAllocator(previous);
    }

    FDefaultAllocatorScope(const FDefaultAllocatorScope&) = delete;
    FDefaultAllocatorScope& operator=(const FDefaultAllocatorScope&) = delete;

private:
    IAllocator* previous;
};

struct FPoolValue {
    explicit FPoolValue(i32 Input = 0) noexcept : value(Input)
    {
    }
    i32 value = 0;
};

#if WITH_RENDER_DX12_RAW
class FOwnershipCommand final : public editor_core::AEditorCommand {
public:
    explicit FOwnershipCommand(i32* Value) noexcept : m_Value(Value)
    {
    }

    void Execute() noexcept override
    {
        if (m_Value != nullptr) ++(*m_Value);
    }

    void Undo() noexcept override
    {
        if (m_Value != nullptr) --(*m_Value);
    }

    const char* Description() const noexcept override
    {
        return "Ownership";
    }

private:
    i32* m_Value = nullptr;
};
#endif

} // namespace

ACS_TEST(AllocatorOwnership, ParticlePoolReturnsToOriginalAllocator)
{
    FOwnershipAllocator First;
    FOwnershipAllocator Second;
    FDefaultAllocatorScope RestoreScope(First);

    CParticleSystem Particles;
    const auto InitializationResult = Particles.Init(64);
    EXPECT_TRUE(InitializationResult.IsOk());
    EXPECT_TRUE(First.outstanding_allocations > 0u);

    SetDefaultAllocator(&Second);
    Particles.Shutdown();

    EXPECT_EQ(First.outstanding_allocations, 0u);
    EXPECT_EQ(First.outstanding_bytes, 0ull);
    EXPECT_EQ(Second.foreign_free_count, 0u);
}

ACS_TEST(AllocatorOwnership, ParticleInitFailureLeavesEmptyState)
{
    FOwnershipAllocator FailingAllocator;
    FailingAllocator.fail_allocations = true;
    FDefaultAllocatorScope RestoreScope(FailingAllocator);

    CParticleSystem Particles;
    const auto InitializationResult = Particles.Init(64);
    EXPECT_TRUE(InitializationResult.IsErr());
    EXPECT_EQ(Particles.Capacity(), 0u);
    EXPECT_EQ(Particles.ActiveCount(), 0u);
    Particles.Shutdown();
    EXPECT_EQ(FailingAllocator.outstanding_allocations, 0u);
}

ACS_TEST(AllocatorOwnership, DevConsoleUsesConstructionAllocator)
{
    FOwnershipAllocator First;
    FOwnershipAllocator Second;
    {
        CDevConsole Console(First);
        Console.PushHistory("allocator ownership");
        Console.Log("diagnostic line");
        EXPECT_TRUE(First.outstanding_allocations > 0u);

        FDefaultAllocatorScope RestoreScope(Second);
        Console.Clear();
        EXPECT_EQ(Second.foreign_free_count, 0u);
    }

    EXPECT_EQ(First.outstanding_allocations, 0u);
    EXPECT_EQ(First.outstanding_bytes, 0ull);
    EXPECT_EQ(Second.foreign_free_count, 0u);
}

ACS_TEST(AllocatorOwnership, ObjectPoolReleasesAllStorageAndInvalidatesHandles)
{
    FOwnershipAllocator Allocator;
    TObjectPool<FPoolValue> Pool(Allocator);

    const FObjectHandle FirstHandle = Pool.Create(7);
    EXPECT_TRUE(Pool.Destroy(FirstHandle));
    const FObjectHandle SecondHandle = Pool.Create(11);
    EXPECT_TRUE(Pool.Destroy(SecondHandle));
    const FObjectHandle OldHandle = Pool.Create(17);
    for (u32 i = 0; i < TObjectPool<FPoolValue>::kChunkSize + 8u; ++i) {
        EXPECT_TRUE(Pool.Create(static_cast<i32>(i)).IsSet());
    }
    EXPECT_TRUE(Allocator.outstanding_allocations > 0u);

    Pool.ReleaseStorage();
    EXPECT_EQ(Allocator.outstanding_allocations, 0u);
    EXPECT_EQ(Allocator.outstanding_bytes, 0ull);
    EXPECT_TRUE(Pool.Get(FirstHandle) == nullptr);
    EXPECT_TRUE(Pool.Get(SecondHandle) == nullptr);
    EXPECT_TRUE(Pool.Get(OldHandle) == nullptr);

    const FObjectHandle NewHandle = Pool.Create(23);
    EXPECT_TRUE(NewHandle.IsSet());
    EXPECT_TRUE(NewHandle != FirstHandle);
    EXPECT_TRUE(NewHandle != SecondHandle);
    EXPECT_TRUE(NewHandle != OldHandle);
    EXPECT_TRUE(Pool.Get(FirstHandle) == nullptr);
    EXPECT_TRUE(Pool.Get(SecondHandle) == nullptr);
    EXPECT_TRUE(Pool.Get(OldHandle) == nullptr);
    EXPECT_EQ(Pool.Get(NewHandle)->value, 23);

    Pool.ReleaseStorage();
    EXPECT_EQ(Allocator.outstanding_allocations, 0u);
    EXPECT_EQ(Allocator.outstanding_bytes, 0ull);
    EXPECT_EQ(Allocator.foreign_free_count, 0u);
}

#if WITH_RENDER_DX12_RAW
ACS_TEST(AllocatorOwnership, UndoStackKeepsCommandAllocator)
{
    FOwnershipAllocator First;
    FOwnershipAllocator Second;
    i32 Value = 0;

    {
        editor_core::CUndoStack Stack;
        auto Command = MakeUniqueIn<FOwnershipCommand>(First, &Value);
        EXPECT_TRUE(static_cast<bool>(Command));
        Stack.Push(TUniquePtr<editor_core::AEditorCommand>(Move(Command)));
        EXPECT_EQ(Value, 1);

        FDefaultAllocatorScope RestoreScope(Second);
        Stack.Clear();
        EXPECT_EQ(Second.foreign_free_count, 0u);
    }

    EXPECT_EQ(First.outstanding_allocations, 0u);
    EXPECT_EQ(First.outstanding_bytes, 0ull);
    EXPECT_EQ(Second.foreign_free_count, 0u);
}
#endif

ACS_TEST(AllocatorOwnership, EasyClosureReturnsToCreationAllocator)
{
    FOwnershipAllocator First;
    FOwnershipAllocator Second;
    FDefaultAllocatorScope RestoreScope(First);
    i32 InvocationCount = 0;

    auto* const Closure = easy::jobdetail::MakeClosure([&InvocationCount]() noexcept { ++InvocationCount; });
    EXPECT_TRUE(Closure != nullptr);
    if (Closure == nullptr) return;
    EXPECT_TRUE(First.outstanding_allocations > 0u);

    Closure->invoke(Closure);
    EXPECT_EQ(InvocationCount, 1);
    SetDefaultAllocator(&Second);
    easy::jobdetail::DestroyClosure(Closure);

    EXPECT_EQ(First.outstanding_allocations, 0u);
    EXPECT_EQ(First.outstanding_bytes, 0ull);
    EXPECT_EQ(Second.foreign_free_count, 0u);
}
