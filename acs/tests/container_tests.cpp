// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "container/Span.h"
#include "container/HashMap.h"
#include "container/Hash.h"
#include "foundation/Move.h"
#include "memory/SystemAllocator.h"
#include "platform/Storage.h"

using namespace acs;

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

    Storage storage(storage_allocator);
    storage.SetString("long.key.for.allocator.contract", "a value long enough to require heap backed FString storage");
    EXPECT_TRUE(storage.Has("long.key.for.allocator.contract"));
    EXPECT_TRUE(storage_allocator.BytesAllocated() > 0);
    EXPECT_EQ(unrelated_allocator.BytesAllocated(), 0ull);

    Storage moved(Move(storage));
    moved.SetString("second.long.key.for.allocator.contract",
                    "another value that must use the original explicit allocator");
    EXPECT_TRUE(moved.Has("second.long.key.for.allocator.contract"));
    EXPECT_EQ(unrelated_allocator.BytesAllocated(), 0ull);

    moved.Clear();
    EXPECT_EQ(moved.Count(), static_cast<usize>(0));
    EXPECT_EQ(storage_allocator.BytesAllocated(), 0ull);
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
