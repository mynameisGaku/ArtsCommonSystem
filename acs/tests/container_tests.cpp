// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "container/Span.h"
#include "container/HashMap.h"
#include "container/Hash.h"
#include "container/InlineArray.h"
#include "container/StableStringKey.h"
#include "container/StringHasher.h"
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

/** Alloc/Realloc/Free の経路と失敗時 rollback を観測する backing。 */
class FCountingAllocator final : public FAllocator {
public:
    /** Realloc を意図的に失敗させるかを切り替える。 */
    void SetFailRealloc(bool bFail) noexcept { m_bFailRealloc = bFail; }

    /** 指定した通算 Alloc 呼び出しだけを失敗させる。0 は無効。 */
    void SetFailAllocCall(u64 Call) noexcept { m_FailAllocCall = Call; }

    /** Alloc 呼び出しを計数して backing へ転送する。 */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++AllocCalls;
        if (AllocCalls == m_FailAllocCall) return nullptr;
        return Backing.Alloc(Size, Alignment, Location);
    }

    /** Realloc 呼び出しを計数し、失敗設定を反映して backing へ転送する。 */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++ReallocCalls;
        if (m_bFailRealloc) return nullptr;
        return Backing.Realloc(Pointer, OldSize, NewSize, Alignment, Location);
    }

    /** Free 呼び出しを計数して backing へ転送する。 */
    void Free(void* Pointer) noexcept override
    {
        if (Pointer != nullptr) ++FreeCalls;
        Backing.Free(Pointer);
    }

    /** Alloc 呼び出し回数。 */
    u64 AllocCalls = 0u;

    /** Realloc 呼び出し回数。 */
    u64 ReallocCalls = 0u;

    /** 非 nullptr の Free 呼び出し回数。 */
    u64 FreeCalls = 0u;

private:
    /** 実際の確保を担当する allocator。 */
    FSystemAllocator Backing;

    /** Realloc を失敗させる場合は true。 */
    bool m_bFailRealloc = false;

    /** 失敗させる通算 Alloc 呼び出し。0 は無効。 */
    u64 m_FailAllocCall = 0u;
};

/** 非 trivial だが byte relocation 契約を満たすテスト値。 */
struct FExplicitlyRelocatableValue {
    /** 計数先と保持値を設定して構築する。 */
    explicit FExplicitlyRelocatableValue(usize& InMoveCount, usize& InDestructionCount, i32 InValue) noexcept : MoveCount(&InMoveCount), DestructionCount(&InDestructionCount), Value(InValue)
    {
    }

    /** コピー構築を禁止する。 */
    FExplicitlyRelocatableValue(const FExplicitlyRelocatableValue&) = delete;

    /** コピー代入を禁止する。 */
    FExplicitlyRelocatableValue& operator=(const FExplicitlyRelocatableValue&) = delete;

    /** 明示的なムーブ経路を計数して構築する。 */
    FExplicitlyRelocatableValue(FExplicitlyRelocatableValue&& Other) noexcept : MoveCount(Other.MoveCount), DestructionCount(Other.DestructionCount), Value(Other.Value)
    {
        ++*MoveCount;
        Other.Value = -1;
    }

    /** 破棄回数を計数する。 */
    ~FExplicitlyRelocatableValue() noexcept
    {
        ++*DestructionCount;
    }

    /** ムーブ構築回数の出力先。 */
    usize* MoveCount = nullptr;

    /** 破棄回数の出力先。 */
    usize* DestructionCount = nullptr;

    /** テスト用の保持値。 */
    i32 Value = 0;
};

/** 全キーを同じ bucket 列へ集め、hash 呼び出し回数も計測する。 */
struct FConstantCountingHasher {
    /** 固定 hash を返して呼び出し回数を増やす。 */
    u64 operator()(u32 /*Key*/) const noexcept
    {
        ++CallCount;
        return 0xA500000000000007ull;
    }

    /** 全インスタンスで共有する呼び出し回数。 */
    inline static usize CallCount = 0u;
};

} // namespace

namespace acs {

template<>
struct TIsTriviallyRelocatable<FExplicitlyRelocatableValue> : TTrueType {};

} // namespace acs

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

ACS_TEST(Container, ArrayRelocatableTraitUsesReallocAndPreservesRollback)
{
    static_assert(IsTriviallyRelocatableV<u32>);
    static_assert(!IsTriviallyRelocatableV<FArrayTrackedValue>);
    static_assert(IsTriviallyRelocatableV<FExplicitlyRelocatableValue>);

    // 確保経路を観測する allocator。
    FCountingAllocator Counting;
    // trivial relocation 経路を確認する配列。
    TArray<u32> Values(Counting);
    EXPECT_TRUE(Values.TryReserve(1u));
    EXPECT_TRUE(Values.TryPushBack(41u));
    // 最初の成長前に使われていた格納先。
    u32* const BeforeGrowth = Values.Data();

    EXPECT_TRUE(Values.TryPushBack(73u));
    EXPECT_EQ(Counting.AllocCalls, 1ull);
    EXPECT_EQ(Counting.ReallocCalls, 1ull);
    EXPECT_EQ(Values[0], 41u);
    EXPECT_EQ(Values[1], 73u);
    EXPECT_TRUE(Values.Data() != nullptr);
    (void)BeforeGrowth;

    // 失敗前の格納先。
    u32* const BeforeFailure = Values.Data();
    // 失敗前の容量。
    const usize CapacityBeforeFailure = Values.Capacity();
    Counting.SetFailRealloc(true);
    while (Values.Size() < Values.Capacity()) {
        EXPECT_TRUE(Values.TryPushBack(static_cast<u32>(Values.Size())));
    }
    // 失敗前の要素数。
    const usize SizeBeforeFailure = Values.Size();
    // 失敗前の先頭値。
    const u32 FirstBeforeFailure = Values[0];
    EXPECT_FALSE(Values.TryPushBack(999u));
    EXPECT_EQ(Values.Data(), BeforeFailure);
    EXPECT_EQ(Values.Size(), SizeBeforeFailure);
    EXPECT_EQ(Values.Capacity(), CapacityBeforeFailure);
    EXPECT_EQ(Values[0], FirstBeforeFailure);

    // 明示 relocation 型のムーブ回数。
    usize MoveCount = 0u;
    // 明示 relocation 型の破棄回数。
    usize DestructionCount = 0u;
    {
        // byte relocation opt-in 型の配列。
        TArray<FExplicitlyRelocatableValue> Relocatable;
        EXPECT_TRUE(Relocatable.TryReserve(1u));
        EXPECT_TRUE(Relocatable.TryEmplaceBack(MoveCount, DestructionCount, 17) != nullptr);
        EXPECT_TRUE(Relocatable.TryEmplaceBack(MoveCount, DestructionCount, 29) != nullptr);
        EXPECT_EQ(MoveCount, static_cast<usize>(0));
        EXPECT_EQ(DestructionCount, static_cast<usize>(0));
        EXPECT_EQ(Relocatable[0].Value, 17);
        EXPECT_EQ(Relocatable[1].Value, 29);
    }
    EXPECT_EQ(MoveCount, static_cast<usize>(0));
    EXPECT_EQ(DestructionCount, static_cast<usize>(2));
}

ACS_TEST(Container, HashMapCollisionUpdateAndStableLookupContracts)
{
    // map の確保回数を観測する allocator。
    FCountingAllocator Counting;
    // 全キーを同じ探索列へ集める map。
    THashMap<u32, u32, FConstantCountingHasher> Colliding(Counting);
    EXPECT_TRUE(Colliding.TryReserve(96u));
    for (u32 Key = 0u; Key < 64u; ++Key) {
        EXPECT_TRUE(Colliding.TryInsert(Key, Key * 3u));
    }
    for (u32 Key = 0u; Key < 64u; ++Key) {
        // 衝突探索で見つけた値。
        const u32* const Value = Colliding.Find(Key);
        EXPECT_TRUE(Value != nullptr);
        if (Value != nullptr) EXPECT_EQ(*Value, Key * 3u);
    }

    // 既存キー更新直前の確保回数。
    const u64 AllocCallsBeforeUpdate = Counting.AllocCalls;
    FConstantCountingHasher::CallCount = 0u;
    EXPECT_TRUE(Colliding.TryInsert(31u, 999u));
    EXPECT_EQ(FConstantCountingHasher::CallCount, static_cast<usize>(1));
    EXPECT_EQ(Counting.AllocCalls, AllocCallsBeforeUpdate);
    EXPECT_EQ(*Colliding.Find(31u), 999u);

    // constexpr と runtime の短い ASCII 入力。
    constexpr char ShortLiteral[] = "stable";
    // 32 byte 超の constexpr 入力。
    constexpr char LongLiteral[] = "0123456789abcdefghijklmnopqrstuvwxyz-constexpr";
    // 埋め込み NUL を含む constexpr 入力。
    constexpr char EmbeddedLiteral[] = {'a', '\0', 'b', '\0'};
    static_assert(HashLiteral(ShortLiteral) == HashBytesConstexpr(ShortLiteral, sizeof(ShortLiteral) - 1u));
    static_assert(HashLiteral(LongLiteral) == HashBytesConstexpr(LongLiteral, sizeof(LongLiteral) - 1u));
    static_assert(HashLiteral(EmbeddedLiteral) == HashBytesConstexpr(EmbeddedLiteral, sizeof(EmbeddedLiteral) - 1u));
    static_assert(HashBytesConstexpr(nullptr, 1u) == 0u);
    EXPECT_EQ(HashLiteral(ShortLiteral), HashBytes(ShortLiteral, sizeof(ShortLiteral) - 1u));
    EXPECT_EQ(HashLiteral(LongLiteral), HashBytes(LongLiteral, sizeof(LongLiteral) - 1u));
    EXPECT_EQ(HashLiteral(EmbeddedLiteral), HashBytes(EmbeddedLiteral, sizeof(EmbeddedLiteral) - 1u));
    EXPECT_EQ(HashBytesConstexpr(nullptr, 0u), HashBytes(nullptr, 0u));
    EXPECT_EQ(HashBytes(nullptr, 1u), 0ull);

    // heterogeneous 検索対象の文字列 map。
    THashMap<FString, u32, FStringHasher> Strings;
    // 通常 ASCII キー。
    FString Stable("stable");
    // 埋め込み NUL キー。
    FString Embedded(FStringView(EmbeddedLiteral, sizeof(EmbeddedLiteral) - 1u));
    EXPECT_TRUE(Strings.TryInsert(Stable, 7u));
    EXPECT_TRUE(Strings.TryInsert(Embedded, 11u));
    EXPECT_EQ(*Strings.FindAs(FStringView("stable")), 7u);
    // 短い文字列のコンパイル時 hash key。
    constexpr FStableStringKey StableKey = MakeStableStringKey("stable");
    EXPECT_EQ(*Strings.FindByHash(StableKey.View, StableKey.Hash), 7u);
    // 埋め込み NUL キーの事前計算 hash view。
    const FStableStringKey EmbeddedKey{FStringView(EmbeddedLiteral, sizeof(EmbeddedLiteral) - 1u), HashLiteral(EmbeddedLiteral)};
    EXPECT_EQ(*Strings.FindByHash(EmbeddedKey.View, EmbeddedKey.Hash), 11u);
    EXPECT_TRUE(Strings.FindByHash(FStringView("stable"), HashLiteral("different")) == nullptr);

    // stable key 自体を K にしても Find overload が衝突しないことを型生成で確認する。
    static_assert(sizeof(THashMap<FStableStringKey, u32>) > 0u);
}

/** HashMap の段階的な拡張失敗後も既存内容と検索不変条件が保たれることを検証する。 */
ACS_TEST(Container, HashMapGrowthFailurePreservesContentsAndSearchIntegrity)
{
    {
        // 2 回目の Alloc、すなわち初回値配列確保だけを失敗させる allocator。
        FCountingAllocator Allocator;
        Allocator.SetFailAllocCall(2u);
        // 初回 rehash 成功後の値配列 OOM を再現する map。
        THashMap<u32, u32> Map(Allocator);

        EXPECT_FALSE(Map.TryInsert(7u, 70u));
        EXPECT_EQ(Map.Size(), static_cast<usize>(0));
        EXPECT_TRUE(Map.Find(7u) == nullptr);
        EXPECT_EQ(Allocator.AllocCalls, 2ull);

        // 失敗前に確保済みの bucket 容量を再利用するため、retry は値配列の 1 確保だけになる。
        const u64 CallsAfterFailure = Allocator.AllocCalls;
        Allocator.SetFailAllocCall(0u);
        EXPECT_TRUE(Map.TryInsert(7u, 70u));
        EXPECT_EQ(Allocator.AllocCalls - CallsAfterFailure, 1ull);
        EXPECT_EQ(Map.Size(), static_cast<usize>(1));
        // retry 後に検索した値。
        const u32* const RetriedValue = Map.Find(7u);
        EXPECT_TRUE(RetriedValue != nullptr);
        if (RetriedValue != nullptr) EXPECT_EQ(*RetriedValue, 70u);
    }

    {
        // populated map の Realloc 失敗を注入する allocator。
        FCountingAllocator Allocator;
        // 値配列容量 8 を満たした状態から次の成長を試す map。
        THashMap<u32, u32> Map(Allocator);
        for (u32 Key = 0u; Key < 8u; ++Key) {
            EXPECT_TRUE(Map.TryInsert(Key, Key + 100u));
        }

        Allocator.SetFailRealloc(true);
        EXPECT_FALSE(Map.TryInsert(8u, 108u));
        EXPECT_EQ(Map.Size(), static_cast<usize>(8));
        EXPECT_TRUE(Map.Find(8u) == nullptr);
        for (u32 Key = 0u; Key < 8u; ++Key) {
            const u32* const Value = Map.Find(Key);
            EXPECT_TRUE(Value != nullptr);
            if (Value != nullptr) EXPECT_EQ(*Value, Key + 100u);
        }

        Allocator.SetFailRealloc(false);
        EXPECT_TRUE(Map.TryInsert(8u, 108u));
        // retry 後に追加できた値。
        const u32* const AddedValue = Map.Find(8u);
        EXPECT_TRUE(AddedValue != nullptr);
        if (AddedValue != nullptr) EXPECT_EQ(*AddedValue, 108u);
    }
}

ACS_TEST(Container, StringByteSearchAppendAndCompareContracts)
{
    // NUL と UTF-8 byte 列を含む検索対象。
    constexpr char HaystackBytes[] = {'A', '\0', 'B', static_cast<char>(0xE3), static_cast<char>(0x81), static_cast<char>(0x82), 'Z'};
    // 埋め込み NUL から始まる検索語。
    constexpr char NeedleBytes[] = {'\0', 'B'};
    // byte 長を保持する検索対象 view。
    const FStringView Haystack(HaystackBytes, sizeof(HaystackBytes));
    // byte 長を保持する検索語 view。
    const FStringView Needle(NeedleBytes, sizeof(NeedleBytes));
    EXPECT_EQ(Haystack.Find(Needle), static_cast<usize>(1));
    EXPECT_TRUE(Haystack.Contains(Needle));
    EXPECT_TRUE(Haystack.Compare(FStringView("A", 1u)) > 0);
    EXPECT_TRUE(FStringView("abc", 3u).Compare(FStringView("abd", 3u)) < 0);

    // 自己参照 append の対象文字列。
    FString Self;
    Self.Append("0123456789abcdefghijklmnopqrstuv");
    // Self 内部を参照する追記元。
    const FStringView Tail = Self.View().SubView(8u, 12u);
    EXPECT_TRUE(Self.TryAppend(Tail));
    EXPECT_TRUE(Self.EndsWith(FStringView("89abcdefghij", 12u)));

    // 失敗注入 allocator の backing。
    FSystemAllocator Backing;
    // 次回確保を失敗させられる allocator。
    FSwitchableFailAllocator Failing(Backing);
    // 失敗時 rollback を確認する文字列。
    FString Rollback(Failing);
    Rollback.Append("0123456789abcdefghijklmnopqrstuv");
    while (Rollback.Size() < Rollback.Capacity()) {
        EXPECT_TRUE(Rollback.TryAppend('x'));
    }
    // 失敗前の文字列内容。
    const FString BeforeFailure(Rollback);
    // 失敗前の格納先。
    const char* const DataBeforeFailure = Rollback.Data();
    Failing.SetFailing(true);
    EXPECT_FALSE(Rollback.TryAppend('y'));
    EXPECT_EQ(Rollback.Data(), DataBeforeFailure);
    EXPECT_TRUE(Rollback == BeforeFailure);
}
