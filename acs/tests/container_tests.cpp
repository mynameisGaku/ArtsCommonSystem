// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "container/HashMap.h"
#include "container/Hash.h"
#include "foundation/Move.h"
#include "memory/SystemAllocator.h"
#include "platform/Storage.h"

using namespace acs;

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
