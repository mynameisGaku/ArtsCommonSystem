// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "container/Span.h"
#include "container/HashMap.h"
#include "container/Hash.h"
#include "container/HashBytesBatch.h"
#include "container/InlineArray.h"
#include "foundation/Move.h"
#include "memory/SystemAllocator.h"
#include "platform/Storage.h"

using namespace acs;

ACS_TEST(Container, InlineArrayAvoidsHeapUntilCapacityAndPreservesOrder)
{
    FSystemAllocator allocator;
    TInlineArray<u32, 4u> values(allocator);
    const u64 initial_allocations = allocator.AllocationCount();
    for (u32 i = 0u; i < 4u; ++i) values.PushBack(i + 10u);
    EXPECT_TRUE(values.UsesInlineStorage());
    EXPECT_EQ(allocator.AllocationCount(), initial_allocations);

    values.PushBack(14u);
    EXPECT_FALSE(values.UsesInlineStorage());
    EXPECT_TRUE(allocator.AllocationCount() > initial_allocations);
    EXPECT_EQ(values.Size(), static_cast<usize>(5u));
    for (u32 i = 0u; i < 5u; ++i) EXPECT_EQ(values[i], i + 10u);

    const u64 spilled_allocations = allocator.AllocationCount();
    values.Clear();
    values.PushBack(99u);
    EXPECT_FALSE(values.UsesInlineStorage());
    EXPECT_EQ(allocator.AllocationCount(), spilled_allocations);
    EXPECT_EQ(values[0], 99u);
}

ACS_TEST(Container, BatchHashMaintainsScalarParity)
{
    const char short_text[] = "acs";
    const char medium_text[] = "foundation optimization";
    const char empty_text[] = "";
    byte binary[65]{};
    for (u32 i = 0u; i < 65u; ++i)
        binary[i] = static_cast<byte>(i * 17u + 3u);
    const FHashBytesInput inputs[] = {{short_text, sizeof(short_text) - 1u, 1u}, {medium_text, sizeof(medium_text) - 1u, 2u}, {binary, sizeof(binary), 3u}, {empty_text, 0u, 4u}};
    u64 batch[4]{};
    HashBytesBatch(inputs, 4u, batch);
    for (usize i = 0u; i < 4u; ++i) {
        EXPECT_EQ(batch[i], HashBytes(inputs[i].data, inputs[i].length, inputs[i].seed));
    }

    const u64 keys[4] = {0u, 1u, 0x123456789abcdef0ull, ~0ull};
    u64 mixed[4]{};
    HashMix64Batch4(keys, mixed);
    for (u32 i = 0u; i < 4u; ++i)
        EXPECT_EQ(mixed[i], HashMix64(keys[i]));
}

namespace {

/** 確保失敗時のコンテナ契約を検証する backing。 */
class FAlwaysFailAllocator final : public FAllocator {
public:
    void* Alloc(usize /*Size*/, usize /*Alignment*/, FSourceLoc /*Location*/) noexcept override
    {
        return nullptr;
    }

    void Free(void* /*Pointer*/) noexcept override
    {
    }
};

/** 実 backing へ委譲しつつ、フラグを立てた後の確保だけ失敗させる backing。 */
class FSwitchableFailAllocator final : public FAllocator {
public:
    explicit FSwitchableFailAllocator(FAllocator& Backing) noexcept : m_Backing(&Backing)
    {
    }

    void SetFailing(bool bFailing) noexcept
    {
        m_bFailing = bFailing;
    }

    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        return m_bFailing ? nullptr : m_Backing->Alloc(Size, Alignment, Location);
    }

    void Free(void* Pointer) noexcept override
    {
        m_Backing->Free(Pointer);
    }

private:
    FAllocator* m_Backing = nullptr;
    bool m_bFailing = false;
};

/** 指定した確保要求だけを失敗させ、各commit段階を個別に検証するbacking。 */
class FFailOnRequestAllocator final : public FAllocator {
public:
    void FailOnRequest(u64 Request) noexcept
    {
        m_RequestCount = 0;
        m_FailingRequest = Request;
    }

    void DisableFailure() noexcept
    {
        m_RequestCount = 0;
        m_FailingRequest = 0;
    }

    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++m_RequestCount;
        if (m_FailingRequest != 0 && m_RequestCount == m_FailingRequest) {
            return nullptr;
        }
        return m_Backing.Alloc(Size, Alignment, Location);
    }

    void Free(void* Pointer) noexcept override
    {
        m_Backing.Free(Pointer);
    }

    u64 BytesAllocated() const noexcept override
    {
        return m_Backing.BytesAllocated();
    }

private:
    FSystemAllocator m_Backing;
    u64 m_RequestCount = 0;
    u64 m_FailingRequest = 0;
};

/**
 * 解放要求を受けた領域を poison してテスト終了まで保持する backing。
 *
 * @details grow 後に旧領域の参照を読んでしまう回帰を、偶然メモリが残ることに
 * 依存せず検出する。コンテナからの Free は記録するが、実 backing への返却は
 * この allocator の破棄時まで遅延する。
 */
class FQuarantiningAllocator final : public FAllocator {
public:
    ~FQuarantiningAllocator() noexcept override
    {
        for (usize Index = 0u; Index < m_AllocationCount; ++Index) {
            m_Backing.Free(m_Allocations[Index].Pointer);
        }
    }

    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (m_AllocationCount >= kMaximumAllocations) return nullptr;
        void* const Pointer = m_Backing.Alloc(Size, Alignment, Location);
        if (!Pointer) return nullptr;

        m_Allocations[m_AllocationCount++] = {
            Pointer,
            Size,
            false
        };
        ++m_LiveContainerAllocations;
        return Pointer;
    }

    void Free(void* Pointer) noexcept override
    {
        if (!Pointer) return;
        for (usize Index = 0u; Index < m_AllocationCount; ++Index) {
            FAllocation& Allocation = m_Allocations[Index];
            if (Allocation.Pointer != Pointer || Allocation.bReleased) continue;

            MemSet(Allocation.Pointer, 0xA5, Allocation.Size);
            Allocation.bReleased = true;
            --m_LiveContainerAllocations;
            return;
        }
    }

    usize LiveContainerAllocations() const noexcept
    {
        return m_LiveContainerAllocations;
    }

private:
    struct FAllocation {
        void* Pointer = nullptr;
        usize Size = 0u;
        bool bReleased = false;
    };

    static constexpr usize kMaximumAllocations = 8u;
    FSystemAllocator m_Backing;
    FAllocation m_Allocations[kMaximumAllocations]{};
    usize m_AllocationCount = 0u;
    usize m_LiveContainerAllocations = 0u;
};

struct FArrayValueCounters {
    usize ValueConstructions = 0u;
    usize CopyConstructions = 0u;
    usize MoveConstructions = 0u;
    usize Destructions = 0u;
    usize LiveValues = 0u;
};

/** 非 trivial な値型。grow の構築順序と失敗時 rollback を観測する。 */
struct FArrayTrackedValue {
    static constexpr int kMovedFromValue = -777777;

    explicit FArrayTrackedValue(
        FArrayValueCounters& InCounters,
        int InValue) noexcept
        : Counters(&InCounters),
          Value(InValue)
    {
        ++Counters->ValueConstructions;
        ++Counters->LiveValues;
    }

    FArrayTrackedValue(const FArrayTrackedValue& Other) noexcept
        : Counters(Other.Counters),
          Value(Other.Value)
    {
        ++Counters->CopyConstructions;
        ++Counters->LiveValues;
    }

    FArrayTrackedValue(FArrayTrackedValue&& Other) noexcept
        : Counters(Other.Counters),
          Value(Other.Value)
    {
        Other.Value = kMovedFromValue;
        ++Counters->MoveConstructions;
        ++Counters->LiveValues;
    }

    ~FArrayTrackedValue() noexcept
    {
        ++Counters->Destructions;
        --Counters->LiveValues;
        Value = kMovedFromValue;
    }

    FArrayValueCounters* Counters = nullptr;
    int Value = 0;
};

} // namespace

ACS_TEST(Container, ArrayPushAndIndex) {
    TArray<int> a;
    for (int i = 0; i < 100; ++i) a.PushBack(i);
    EXPECT_EQ(a.Size(), (usize)100);
    EXPECT_EQ(a[0], 0);
    EXPECT_EQ(a[99], 99);
    EXPECT_EQ(a.Back(), 99);
}

ACS_TEST(Container, ArrayResize) {
    TArray<int> a;
    a.Resize(50);
    EXPECT_EQ(a.Size(), (usize)50);
    a.Resize(10);
    EXPECT_EQ(a.Size(), (usize)10);
}

ACS_TEST(Container, ArrayReleaseStoragePreservesAllocator)
{
    TArray<int> a;
    FAllocator* const allocator = a.GetAllocator();
    a.Resize(256);
    EXPECT_TRUE(a.Capacity() >= 256);

    a.ReleaseStorage();
    EXPECT_EQ(a.Size(), static_cast<usize>(0));
    EXPECT_EQ(a.Capacity(), static_cast<usize>(0));
    EXPECT_TRUE(a.Data() == nullptr);
    EXPECT_TRUE(a.GetAllocator() == allocator);

    a.PushBack(42);
    EXPECT_EQ(a[0], 42);
    EXPECT_TRUE(a.GetAllocator() == allocator);
}

ACS_TEST(Container, ArrayTryOperationsPreserveStateOnOverflowAndOutOfMemory)
{
    FAlwaysFailAllocator FailingAllocator;
    TArray<u64> Array(FailingAllocator);

    EXPECT_FALSE(Array.TryReserve(8u));
    EXPECT_FALSE(Array.TryResize(8u));
    EXPECT_FALSE(Array.TryPushBack(42u));
    EXPECT_TRUE(Array.TryEmplaceBack(7u) == nullptr);
    EXPECT_EQ(Array.Size(), static_cast<usize>(0));
    EXPECT_EQ(Array.Capacity(), static_cast<usize>(0));
    EXPECT_TRUE(Array.Data() == nullptr);

    TArray<u64> OverflowArray;
    EXPECT_FALSE(OverflowArray.TryReserve(~usize(0)));
    EXPECT_FALSE(OverflowArray.TryResize(~usize(0)));
    EXPECT_EQ(OverflowArray.Size(), static_cast<usize>(0));
    EXPECT_EQ(OverflowArray.Capacity(), static_cast<usize>(0));
}

ACS_TEST(Container, ArraySelfReferentialGrowthKeepsArgumentsAliveUntilConstruction)
{
    FQuarantiningAllocator Allocator;

    {
        FArrayValueCounters Counters;
        TArray<FArrayTrackedValue> Array(Allocator);
        EXPECT_TRUE(Array.TryReserve(1u));
        EXPECT_TRUE(Array.TryEmplaceBack(Counters, 41) != nullptr);
        FArrayTrackedValue* const OldData = Array.Data();

        EXPECT_TRUE(Array.TryPushBack(Array[0]));
        EXPECT_NE(Array.Data(), OldData);
        EXPECT_EQ(Array.Size(), static_cast<usize>(2));
        EXPECT_EQ(Array[0].Value, 41);
        EXPECT_EQ(Array[1].Value, 41);
        EXPECT_EQ(Counters.CopyConstructions, static_cast<usize>(1));
        EXPECT_EQ(Counters.MoveConstructions, static_cast<usize>(1));
        EXPECT_EQ(Counters.LiveValues, static_cast<usize>(2));
    }
    EXPECT_EQ(
        Allocator.LiveContainerAllocations(),
        static_cast<usize>(0));

    {
        FArrayValueCounters Counters;
        TArray<FArrayTrackedValue> Array(Allocator);
        EXPECT_TRUE(Array.TryReserve(1u));
        EXPECT_TRUE(Array.TryEmplaceBack(Counters, 73) != nullptr);

        EXPECT_TRUE(
            Array.TryEmplaceBack(Counters, Array[0].Value) != nullptr);
        EXPECT_EQ(Array.Size(), static_cast<usize>(2));
        EXPECT_EQ(Array[0].Value, 73);
        EXPECT_EQ(Array[1].Value, 73);
        EXPECT_EQ(Counters.ValueConstructions, static_cast<usize>(2));
        EXPECT_EQ(Counters.MoveConstructions, static_cast<usize>(1));
        EXPECT_EQ(Counters.LiveValues, static_cast<usize>(2));
    }
    EXPECT_EQ(
        Allocator.LiveContainerAllocations(),
        static_cast<usize>(0));

    {
        FArrayValueCounters Counters;
        TArray<FArrayTrackedValue> Array(Allocator);
        EXPECT_TRUE(Array.TryReserve(1u));
        EXPECT_TRUE(Array.TryEmplaceBack(Counters, 95) != nullptr);

        EXPECT_TRUE(Array.TryPushBack(Move(Array[0])));
        EXPECT_EQ(Array.Size(), static_cast<usize>(2));
        EXPECT_EQ(Array[0].Value, FArrayTrackedValue::kMovedFromValue);
        EXPECT_EQ(Array[1].Value, 95);
        EXPECT_EQ(Counters.MoveConstructions, static_cast<usize>(2));
        EXPECT_EQ(Counters.LiveValues, static_cast<usize>(2));
    }
    EXPECT_EQ(
        Allocator.LiveContainerAllocations(),
        static_cast<usize>(0));
}

ACS_TEST(Container, ArraySelfReferentialGrowthRollsBackOnAllocationFailure)
{
    FSystemAllocator Backing;
    FSwitchableFailAllocator Allocator(Backing);
    FArrayValueCounters Counters;

    {
        TArray<FArrayTrackedValue> Array(Allocator);
        EXPECT_TRUE(Array.TryReserve(1u));
        EXPECT_TRUE(Array.TryEmplaceBack(Counters, 127) != nullptr);

        FArrayTrackedValue* const DataBefore = Array.Data();
        const usize SizeBefore = Array.Size();
        const usize CapacityBefore = Array.Capacity();
        const usize ValueConstructionsBefore = Counters.ValueConstructions;
        const usize CopyConstructionsBefore = Counters.CopyConstructions;
        const usize MoveConstructionsBefore = Counters.MoveConstructions;

        // ACS は例外を無効化しているため、grow の例外経路は明示的な
        // allocation failure。確保失敗時は constructor を一度も呼ばない。
        Allocator.SetFailing(true);
        EXPECT_FALSE(Array.TryPushBack(Array[0]));
        EXPECT_FALSE(Array.TryPushBack(Move(Array[0])));
        EXPECT_TRUE(
            Array.TryEmplaceBack(Counters, Array[0].Value) == nullptr);

        EXPECT_TRUE(Array.Data() == DataBefore);
        EXPECT_EQ(Array.Size(), SizeBefore);
        EXPECT_EQ(Array.Capacity(), CapacityBefore);
        EXPECT_EQ(Array[0].Value, 127);
        EXPECT_EQ(
            Counters.ValueConstructions,
            ValueConstructionsBefore);
        EXPECT_EQ(
            Counters.CopyConstructions,
            CopyConstructionsBefore);
        EXPECT_EQ(
            Counters.MoveConstructions,
            MoveConstructionsBefore);
        EXPECT_EQ(Counters.LiveValues, static_cast<usize>(1));

        Allocator.SetFailing(false);
        EXPECT_TRUE(Array.TryPushBack(Array[0]));
        EXPECT_EQ(Array.Size(), static_cast<usize>(2));
        EXPECT_EQ(Array[0].Value, 127);
        EXPECT_EQ(Array[1].Value, 127);
    }

    EXPECT_EQ(Counters.LiveValues, static_cast<usize>(0));
    EXPECT_EQ(Backing.BytesAllocated(), static_cast<u64>(0));
}

ACS_TEST(Container, HashMapTryInsertPreservesStateOnOutOfMemory)
{
    // Part A: 空の map + 常時失敗 backing。最初の TryInsert が bucket 確保 (rehash) で失敗し、
    // map は空・整合のまま。Reserve も false。
    FAlwaysFailAllocator FailingAllocator;
    THashMap<u32, u32> EmptyMap(FailingAllocator);
    EXPECT_FALSE(EmptyMap.TryReserve(8u));
    EXPECT_FALSE(EmptyMap.TryInsert(1u, 100u));
    EXPECT_EQ(EmptyMap.Size(), static_cast<usize>(0));
    EXPECT_FALSE(EmptyMap.Contains(1u));

    // Part B: 実 backing で構築後に確保失敗へ切替え、rehash / 値配列拡張の失敗時も既存
    // エントリを保つ (OOM で map を破壊しない)。
    FSystemAllocator Backing;
    FSwitchableFailAllocator Switchable(Backing);
    THashMap<u32, u32> Map(Switchable);
    for (u32 Key = 0; Key < 10u; ++Key) {
        EXPECT_TRUE(Map.TryInsert(Key, Key * 7u));
    }
    const usize SizeBefore = Map.Size();

    Switchable.SetFailing(true);
    // load factor を超えて rehash / 値配列拡張を強制 → 確保失敗で TryInsert は false を返す。
    bool bRejected = false;
    for (u32 Key = 10u; Key < 4096u && !bRejected; ++Key) {
        bRejected = !Map.TryInsert(Key, Key);
    }
    EXPECT_TRUE(bRejected);

    // 既存エントリは保たれ、Find が正しい値を返す。
    for (u32 Key = 0; Key < 10u; ++Key) {
        const u32* const Value = Map.Find(Key);
        EXPECT_TRUE(Value != nullptr);
        if (Value != nullptr) {
            EXPECT_EQ(*Value, Key * 7u);
        }
    }
    EXPECT_TRUE(Map.Size() >= SizeBefore);
    Switchable.SetFailing(false);
}

ACS_TEST(Container, StringTryAppendPreservesStateOnOutOfMemory)
{
    // SSO 内 (<=22 バイト) の間はヒープ確保が起きないので Try 系は成功する。
    FSystemAllocator Backing;
    FSwitchableFailAllocator Switchable(Backing);
    FString Str(Switchable);
    Str.Append("short");  // SSO
    EXPECT_TRUE(Str.TryAppend("!"));
    EXPECT_EQ(Str.Size(), static_cast<usize>(6));

    // 確保失敗へ切替え。SSO 超過で TryGrow が必要になった時点で TryAppend / TryReserve は
    // false を返し、文字列 (内容・長さ) は変更されない。
    Switchable.SetFailing(true);
    EXPECT_FALSE(Str.TryReserve(4096u));
    const usize SizeBefore = Str.Size();
    bool bRejected = false;
    for (u32 i = 0; i < 512u && !bRejected; ++i) {
        bRejected = !Str.TryAppend("0123456789ABCDEF");
    }
    EXPECT_TRUE(bRejected);
    // 拒否された TryAppend は文字列を壊さない。先頭は "short!" のまま。
    EXPECT_TRUE(Str.Size() >= SizeBefore);
    EXPECT_TRUE(Str.View().StartsWith(FStringView("short!", 6)));

    // AppendFormat も拡張確保が必要な場合は OOM で 0 を返し、文字列を変えない。
    const usize SizeBeforeFormat = Str.Size();
    EXPECT_EQ(Str.AppendFormat("%s", "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"),
              static_cast<usize>(0));
    EXPECT_EQ(Str.Size(), SizeBeforeFormat);

    Switchable.SetFailing(false);
}

ACS_TEST(Container, EmptyArrayIteratorsDoNotPerformNullPointerArithmetic)
{
    TArray<u64> Array;
    EXPECT_TRUE(Array.begin() == nullptr);
    EXPECT_TRUE(Array.end() == Array.begin());

    const TArray<u64>& ConstArray = Array;
    EXPECT_TRUE(ConstArray.begin() == nullptr);
    EXPECT_TRUE(ConstArray.end() == ConstArray.begin());
}

ACS_TEST(Container, EmptyViewsDoNotPerformNullPointerArithmetic)
{
    TSpan<u32> Span;
    EXPECT_TRUE(Span.begin() == nullptr);
    EXPECT_TRUE(Span.end() == Span.begin());
    EXPECT_TRUE(Span.SubSpan(0u, 0u).Data() == nullptr);

    const FStringView StringView;
    EXPECT_TRUE(StringView.begin() == nullptr);
    EXPECT_TRUE(StringView.end() == StringView.begin());
    EXPECT_TRUE(StringView.SubView(0u, 0u).Data() == nullptr);
}

ACS_TEST(Container, StringInlineAndHeap) {
    FString s("hello");
    EXPECT_EQ(s.Size(), (usize)5);
    EXPECT_TRUE(s.View() == FStringView("hello"));

    FString big;
    for (int i = 0; i < 10; ++i) big.Append("0123456789");
    EXPECT_EQ(big.Size(), (usize)100);
}

ACS_TEST(Container, StringFormat) {
    FString s;
    s.AppendFormat("%d-%s", 42, "abc");
    EXPECT_TRUE(s.View() == FStringView("42-abc"));
}

ACS_TEST(Container, StringReleaseStoragePreservesAllocator)
{
    FSystemAllocator allocator;
    FString s("a value long enough to require heap backed FString storage", allocator);
    EXPECT_TRUE(allocator.BytesAllocated() > 0);

    s.ReleaseStorage();
    EXPECT_TRUE(s.IsEmpty());
    EXPECT_TRUE(s.GetAllocator() == &allocator);
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);

    s.Append("another value long enough to require heap backed FString storage");
    EXPECT_TRUE(s.GetAllocator() == &allocator);
    EXPECT_TRUE(allocator.BytesAllocated() > 0);
}

ACS_TEST(Container, StoragePreservesExplicitAllocatorAndReleasesCapacity)
{
    FSystemAllocator storage_allocator;
    FSystemAllocator unrelated_allocator;

    FStorage storage(storage_allocator);
    storage.SetString("long.key.for.allocator.contract", "a value long enough to require heap backed FString storage");
    EXPECT_TRUE(storage.Has("long.key.for.allocator.contract"));
    EXPECT_TRUE(storage_allocator.BytesAllocated() > 0);
    EXPECT_EQ(unrelated_allocator.BytesAllocated(), 0ull);

    FStorage moved(Move(storage));
    moved.SetString("second.long.key.for.allocator.contract",
                    "another value that must use the original explicit allocator");
    EXPECT_TRUE(moved.Has("second.long.key.for.allocator.contract"));
    EXPECT_EQ(unrelated_allocator.BytesAllocated(), 0ull);

    moved.Clear();
    EXPECT_EQ(moved.Count(), static_cast<usize>(0));
    EXPECT_EQ(storage_allocator.BytesAllocated(), 0ull);
}

ACS_TEST(Container, StorageTrySetStringIsAtomicOnAllocationFailure)
{
    constexpr const char* existingKey =
        "existing.key.that.requires.allocator.backed.storage";
    constexpr const char* originalValue =
        "original value that requires allocator backed storage";
    constexpr const char* stableValue =
        "stable value that must remain unchanged after rejection";
    constexpr const char* replacementValue =
        "replacement value that also requires allocator backed storage";

    FFailOnRequestAllocator allocator;
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString(existingKey, originalValue));
    EXPECT_TRUE(storage.TrySetString("stable.key", stableValue));

    bool existingFailureObserved = false;
    bool existingSuccessObserved = false;
    for (u64 request = 1; request <= 16; ++request) {
        const u64 bytesBefore = allocator.BytesAllocated();
        allocator.FailOnRequest(request);
        const bool accepted =
            storage.TrySetString(existingKey, replacementValue);
        allocator.DisableFailure();
        if (accepted) {
            existingSuccessObserved = true;
            break;
        }
        existingFailureObserved = true;
        EXPECT_EQ(storage.Count(), static_cast<usize>(2));
        EXPECT_EQ(allocator.BytesAllocated(), bytesBefore);
        EXPECT_TRUE(FStringView(storage.GetString(existingKey)) ==
                    FStringView(originalValue));
        EXPECT_TRUE(FStringView(storage.GetString("stable.key")) ==
                    FStringView(stableValue));
    }
    EXPECT_TRUE(existingFailureObserved);
    EXPECT_TRUE(existingSuccessObserved);

    // 空のStorageでvalue、key、entry配列の各確保を順番に失敗させる。
    FStorage empty(allocator);
    bool newFailureObserved = false;
    bool newSuccessObserved = false;
    for (u64 request = 1; request <= 16; ++request) {
        const u64 bytesBefore = allocator.BytesAllocated();
        allocator.FailOnRequest(request);
        const bool accepted = empty.TrySetString(
            "new.key.with.a.long.allocator.backed.name",
            "new value that requires allocator backed storage");
        allocator.DisableFailure();
        if (accepted) {
            newSuccessObserved = true;
            break;
        }
        newFailureObserved = true;
        EXPECT_EQ(empty.Count(), static_cast<usize>(0));
        EXPECT_EQ(allocator.BytesAllocated(), bytesBefore);
        EXPECT_FALSE(empty.Has(
            "new.key.with.a.long.allocator.backed.name"));
    }
    EXPECT_TRUE(newFailureObserved);
    EXPECT_TRUE(newSuccessObserved);

    EXPECT_TRUE(storage.TrySetString(
        "spaces", "  keep surrounding spaces  "));
    EXPECT_TRUE(FStringView(storage.GetString("spaces")) ==
                FStringView("  keep surrounding spaces  "));
    EXPECT_TRUE(storage.TrySetString("empty", nullptr));
    EXPECT_TRUE(FStringView(storage.GetString("empty")).IsEmpty());

    constexpr char document[] =
        "loaded.spaces=  keep loaded surrounding spaces  \n";
    FStorage loaded(allocator);
    EXPECT_TRUE(loaded.LoadFromBytes(
        reinterpret_cast<const u8*>(document),
        sizeof(document) - 1u).IsOk());
    EXPECT_TRUE(FStringView(loaded.GetString("loaded.spaces")) ==
                FStringView("  keep loaded surrounding spaces  "));

    EXPECT_TRUE(storage.TrySetString("alias.source", "alias.target"));
    const char* const aliasedKey =
        storage.GetString("alias.source", nullptr);
    EXPECT_TRUE(aliasedKey != nullptr);
    if (aliasedKey != nullptr) {
        EXPECT_TRUE(storage.TrySetString(
            aliasedKey,
            "value copied after the aliased key was resolved"));
    }
    const char* const aliasedValue =
        storage.GetString("alias.target", nullptr);
    EXPECT_TRUE(aliasedValue != nullptr);
    if (aliasedValue != nullptr) {
        EXPECT_TRUE(storage.TrySetString("alias.copy", aliasedValue));
        EXPECT_TRUE(FStringView(storage.GetString("alias.copy")) ==
                    FStringView(
                        "value copied after the aliased key was resolved"));
    }
}

ACS_TEST(Container, StorageLoadFromBytesIsAtomicAndRejectsDuplicateKeys)
{
    constexpr const char* originalKey =
        "original.key.that.requires.allocator.backed.storage";
    constexpr const char* originalValue =
        "original value that must survive every rejected load";
    constexpr const char* stableKey =
        "stable.key.that.requires.allocator.backed.storage";
    constexpr const char* stableValue =
        "stable value that must survive every rejected load";

    FFailOnRequestAllocator allocator;
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString(originalKey, originalValue));
    EXPECT_TRUE(storage.TrySetString(stableKey, stableValue));

    constexpr char duplicateDocument[] =
        "loaded.key.with.allocator.backed.storage=first value\n"
        "another.loaded.key.with.allocator.backed.storage=second value\n"
        "loaded.key.with.allocator.backed.storage=duplicate value\n";
    const u64 duplicateBytesBefore = allocator.BytesAllocated();
    auto duplicateResult = storage.LoadFromBytes(
        reinterpret_cast<const u8*>(duplicateDocument),
        sizeof(duplicateDocument) - 1u);
    EXPECT_TRUE(duplicateResult.IsErr());
    if (duplicateResult.IsErr()) {
        EXPECT_TRUE(
            duplicateResult.Error().category == EErrCategory::Container);
    }
    EXPECT_EQ(storage.Count(), static_cast<usize>(2));
    EXPECT_EQ(allocator.BytesAllocated(), duplicateBytesBefore);
    EXPECT_TRUE(FStringView(storage.GetString(originalKey)) ==
                FStringView(originalValue));
    EXPECT_TRUE(FStringView(storage.GetString(stableKey)) ==
                FStringView(stableValue));

    // key、value、entry 配列の各確保失敗で、公開済み状態を保持する。
    constexpr char validDocument[] =
        "loaded.key.with.allocator.backed.storage=first loaded value\n"
        "another.loaded.key.with.allocator.backed.storage=second loaded value\n";
    bool allocationFailureObserved = false;
    bool successfulLoadObserved = false;
    for (u64 request = 1; request <= 32; ++request) {
        const u64 bytesBefore = allocator.BytesAllocated();
        allocator.FailOnRequest(request);
        auto result = storage.LoadFromBytes(
            reinterpret_cast<const u8*>(validDocument),
            sizeof(validDocument) - 1u);
        allocator.DisableFailure();
        if (result.IsOk()) {
            successfulLoadObserved = true;
            break;
        }

        allocationFailureObserved = true;
        EXPECT_TRUE(result.Error().category == EErrCategory::Memory);
        EXPECT_EQ(storage.Count(), static_cast<usize>(2));
        EXPECT_EQ(allocator.BytesAllocated(), bytesBefore);
        EXPECT_TRUE(FStringView(storage.GetString(originalKey)) ==
                    FStringView(originalValue));
        EXPECT_TRUE(FStringView(storage.GetString(stableKey)) ==
                    FStringView(stableValue));
    }
    EXPECT_TRUE(allocationFailureObserved);
    EXPECT_TRUE(successfulLoadObserved);
    EXPECT_EQ(storage.Count(), static_cast<usize>(2));
    EXPECT_FALSE(storage.Has(originalKey));
    EXPECT_TRUE(FStringView(storage.GetString(
                    "loaded.key.with.allocator.backed.storage")) ==
                FStringView("first loaded value"));
    EXPECT_TRUE(FStringView(storage.GetString(
                    "another.loaded.key.with.allocator.backed.storage")) ==
                FStringView("second loaded value"));

    EXPECT_TRUE(storage.LoadFromBytes(nullptr, 0u).IsOk());
    EXPECT_EQ(storage.Count(), static_cast<usize>(0));
}

ACS_TEST(Container, HashMapInsertFindRemove) {
    THashMap<u32, u32> m;
    for (u32 i = 0; i < 1000; ++i) m.Insert(i, i * 2);
    EXPECT_EQ(m.Size(), (usize)1000);
    for (u32 i = 0; i < 1000; ++i) {
        u32* v = m.Find(i);
        EXPECT_TRUE(v != nullptr);
        if (v) EXPECT_EQ(*v, i * 2);
    }
    EXPECT_TRUE(m.Remove(500));
    EXPECT_TRUE(m.Find(500) == nullptr);
    EXPECT_EQ(m.Size(), (usize)999);
}

ACS_TEST(Container, HashMapReleaseStoragePreservesAllocator)
{
    FSystemAllocator allocator;
    THashMap<u32, u32> map(allocator);
    for (u32 i = 0; i < 64; ++i)
        map.Insert(i, i + 1u);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);

    map.ReleaseStorage();
    EXPECT_TRUE(map.IsEmpty());
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);

    // 解放後の再利用も、構築時と同じアロケータへ戻る。
    map.Insert(7u, 11u);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);
    EXPECT_TRUE(map.Find(7u) != nullptr);
}

ACS_TEST(Container, HashBytesDeterministic) {
    const char* a = "the quick brown fox";
    u64 h1 = HashBytes(a, 19);
    u64 h2 = HashBytes(a, 19);
    EXPECT_EQ(h1, h2);
}

ACS_TEST(Container, ArrayFindIndexOfContains)
{
    TArray<i32> a;
    a.PushBack(3); a.PushBack(1); a.PushBack(4); a.PushBack(1); a.PushBack(5);

    EXPECT_EQ(a.IndexOf(4), static_cast<usize>(2));
    EXPECT_EQ(a.IndexOf(1), static_cast<usize>(1));      // 最初の一致
    EXPECT_EQ(a.IndexOf(9), TArray<i32>::kNpos);
    EXPECT_TRUE(a.Contains(5));
    EXPECT_FALSE(a.Contains(0));

    i32* const p = a.Find(4);
    EXPECT_TRUE(p != nullptr);
    if (p) { *p = 40; }                                   // 可変アクセスできる
    EXPECT_EQ(a[2], 40);
    EXPECT_TRUE(a.Find(999) == nullptr);

    // 述語版。
    EXPECT_EQ(a.IndexOfIf([](const i32& v) { return v > 10; }), static_cast<usize>(2));
    const i32* const q = a.FindIf([](const i32& v) { return v % 5 == 0; });
    EXPECT_TRUE(q != nullptr && *q == 40);
    EXPECT_TRUE(a.FindIf([](const i32& v) { return v < 0; }) == nullptr);
}

ACS_TEST(Container, ArrayRemoveAtAndRemoveFirstSwap)
{
    TArray<i32> a;
    a.PushBack(10); a.PushBack(20); a.PushBack(30); a.PushBack(40);

    // RemoveAt は順序を保つ (O(n) シフト)。
    a.RemoveAt(1);                        // {10, 30, 40}
    EXPECT_EQ(a.Size(), static_cast<usize>(3));
    EXPECT_EQ(a[0], 10);
    EXPECT_EQ(a[1], 30);
    EXPECT_EQ(a[2], 40);

    // RemoveFirstSwap は O(1) だが順序は入れ替わる。
    EXPECT_TRUE(a.RemoveFirstSwap(10));   // {40, 30}
    EXPECT_EQ(a.Size(), static_cast<usize>(2));
    EXPECT_TRUE(a.Contains(30));
    EXPECT_TRUE(a.Contains(40));
    EXPECT_FALSE(a.RemoveFirstSwap(999)); // 見つからなければ false / 変更なし
    EXPECT_EQ(a.Size(), static_cast<usize>(2));
}

ACS_TEST(Container, StringViewFindAndContains)
{
    const FStringView s("hello world.png");
    EXPECT_EQ(s.Find(FStringView("world")), static_cast<usize>(6));
    EXPECT_EQ(s.Find(FStringView("hello")), static_cast<usize>(0));
    EXPECT_EQ(s.Find(FStringView("xyz")), FStringView::kNpos);
    EXPECT_EQ(s.Find(FStringView("png"), 13), FStringView::kNpos);   // from を過ぎた一致は拾わない
    EXPECT_EQ(s.Find('o'), static_cast<usize>(4));
    EXPECT_EQ(s.Find('o', 5), static_cast<usize>(7));
    EXPECT_EQ(s.FindLast('.'), static_cast<usize>(11));
    EXPECT_EQ(s.FindLast('!'), FStringView::kNpos);
    EXPECT_TRUE(s.Contains(FStringView(".png")));
    EXPECT_FALSE(s.Contains(FStringView("jpg")));

    // 空 needle は from を返す (std::string::find と同じ規約)。
    EXPECT_EQ(s.Find(FStringView("")), static_cast<usize>(0));
    EXPECT_EQ(FStringView("").Find(FStringView("")), static_cast<usize>(0));
    EXPECT_EQ(FStringView("").Find(FStringView("a")), FStringView::kNpos);
}

ACS_TEST(Container, StringFindForwarders)
{
    FString str;
    str.Append(FStringView("assets/textures/hero.png"));
    EXPECT_EQ(str.FindLast('/'), static_cast<usize>(15));
    EXPECT_EQ(str.FindLast('.'), static_cast<usize>(20));
    EXPECT_EQ(str.Find(FStringView("textures")), static_cast<usize>(7));
    EXPECT_TRUE(str.Contains(FStringView("hero")));
    EXPECT_TRUE(str.StartsWith(FStringView("assets/")));
    EXPECT_TRUE(str.EndsWith(FStringView(".png")));
    EXPECT_FALSE(str.EndsWith(FStringView(".jpg")));
}
