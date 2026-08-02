// SPDX-License-Identifier: Apache-2.0
// ECS の動作確認テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/TypeTraits.h"
#include "ecs/ComponentRegistry.h"
#include "ecs/World.h"
#include "ecs/Query.h"
#include "ecs/System.h"
#include "ecs/EntityCommandBuffer.h"
#include "ecs/ParallelEntityCommandBuffer.h"
#include "memory/SystemAllocator.h"
#include "threading/ThreadPool.h"

using namespace acs;

/** 旧 ComponentTypeId が正規の実行時番号型と同じ型を参照することを保証する。 */
static_assert(IsSameV<ComponentTypeId, FComponentTypeId>);
/** 旧 ComponentSignatureId が正規の署名型と同じ型を参照することを保証する。 */
static_assert(IsSameV<ComponentSignatureId, FComponentSignatureId>);

namespace {
struct FPosition { f32 x, y, z; };
struct FVelocity { f32 dx, dy, dz; };
struct FHealth   { i32 hp; };
struct FFrozen   { u8 unused; };  // タグ的コンポーネント (除外フィルタ検証用)

static_assert(GetComponentSignatureId<FPosition>() == TComponentTypeTraits<FPosition>::Signature);
static_assert(GetComponentSignatureId<FPosition>() != GetComponentSignatureId<FVelocity>());
static_assert(ecs_detail::HashComponentSignature("") == 0xCBF29CE484222325ull);
static_assert(ecs_detail::HashComponentSignature("a") == 0xAF63DC4C8601EC8Cull);
static_assert(ecs_detail::HashComponentSignature("abc") == 0xE71FA2190541574Bull);
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId __cdecl acs::ecs_detail::StaticComponentSignature(void) [T = Probe]") == ecs_detail::HashComponentSignature("ComponentSignatureId __cdecl acs::ecs_detail::StaticComponentSignature(void) [T = Probe]"));
static_assert(ecs_detail::HashCompatibleComponentSignature("constexpr acs::FComponentSignatureId acs::ecs_detail::StaticComponentSignature() [with T = Probe]") == ecs_detail::HashComponentSignature("constexpr acs::ComponentSignatureId acs::ecs_detail::StaticComponentSignature() [with T = Probe]"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId acs::ecs_detail::StaticComponentSignature() [T = FComponentSignatureId]") == ecs_detail::HashComponentSignature("ComponentSignatureId acs::ecs_detail::StaticComponentSignature() [T = FComponentSignatureId]"));
static_assert(ecs_detail::HashCompatibleComponentSignature("constexpr acs::FComponentSignatureId acs::ecs_detail::StaticComponentSignature() [with T = {anonymous}::FPrefixSignatureProbe; acs::FComponentSignatureId = long unsigned int]") == 0x9F1E5333A2001137ull);
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId __cdecl acs::ecs_detail::StaticComponentSignature(void) [T = (anonymous namespace)::FPrefixSignatureProbe]") == 0x41953EFE5A48533Aull);
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId acs::ecs_detail::StaticComponentSignature() [T = (anonymous namespace)::FPrefixSignatureProbe]") == 0x80F0C817749D3765ull);
/** MSVCでは別名が基底のu64へ展開されるため、互換化は通常ハッシュと同じ結果になる。 */
static_assert(ecs_detail::HashComponentSignature("unsigned __int64 __cdecl acs::ecs_detail::StaticComponentSignature<struct FProbe>(void) noexcept") == 0x15170512DE8868BFull);
static_assert(ecs_detail::HashCompatibleComponentSignature("unsigned __int64 __cdecl acs::ecs_detail::StaticComponentSignature<struct FProbe>(void) noexcept") == 0x15170512DE8868BFull);
static_assert(ecs_detail::HashCompatibleComponentSignature("AFComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("AFComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("_FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("_FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("$FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("$FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("型FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("型FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId2 acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("FComponentSignatureId2 acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureIdIdentifier acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("FComponentSignatureIdIdentifier acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId$ acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("FComponentSignatureId$ acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId型 acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("FComponentSignatureId型 acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("prefix FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("prefix FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("foo::FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("foo::FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("xacs::FComponentSignatureId acs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("xacs::FComponentSignatureId acs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId Xacs::ecs_detail::StaticComponentSignature()") == ecs_detail::HashComponentSignature("FComponentSignatureId Xacs::ecs_detail::StaticComponentSignature()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId acs::ecs_detail::StaticComponentSignatureExtra()") == ecs_detail::HashComponentSignature("FComponentSignatureId acs::ecs_detail::StaticComponentSignatureExtra()"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureId") == ecs_detail::HashComponentSignature("FComponentSignatureId"));
static_assert(ecs_detail::HashCompatibleComponentSignature("F") == ecs_detail::HashComponentSignature("F"));
static_assert(ecs_detail::HashCompatibleComponentSignature("FComponentSignatureI") == ecs_detail::HashComponentSignature("FComponentSignatureI"));
static_assert(ecs_detail::HashCompatibleComponentSignature("ComponentSignatureId Probe") == ecs_detail::HashComponentSignature("ComponentSignatureId Probe"));

/** 非コピー・可ムーブなコンポーネント (World::CopyFrom の拒否契約検証用)。 */
struct FMoveOnlyComp {
    u32 v = 0;
    FMoveOnlyComp() = default;
    explicit FMoveOnlyComp(u32 x) noexcept : v(x) {}
    FMoveOnlyComp(const FMoveOnlyComp&) = delete;
    FMoveOnlyComp& operator=(const FMoveOnlyComp&) = delete;
    FMoveOnlyComp(FMoveOnlyComp&&) noexcept = default;
    FMoveOnlyComp& operator=(FMoveOnlyComp&&) noexcept = default;
};

/** 常に確保失敗する backing (command buffer の OOM 契約検証用)。 */
class FEcbFailAllocator final : public IAllocator {
public:
    void* Alloc(usize /*Size*/, usize /*Alignment*/, FSourceLoc /*Location*/) noexcept override
    {
        return nullptr;
    }

    void Free(void* /*Pointer*/) noexcept override
    {
    }
};
}

ACS_TEST(Ecs, CreateDestroy) {
    CWorld w;
    FEntityId a = w.Create();
    FEntityId b = w.Create();
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
    EXPECT_EQ(w.EntityCount(), 2u);
    w.Destroy(a);
    EXPECT_FALSE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
}

ACS_TEST(Ecs, AddGetRemoveComponent) {
    CWorld w;
    FEntityId e = w.Create();
    w.Add<FPosition>(e, {1, 2, 3});
    EXPECT_TRUE(w.Has<FPosition>(e));
    FPosition* p = w.Get<FPosition>(e);
    EXPECT_TRUE(p != nullptr);
    if (p) EXPECT_NEAR(p->x, 1.0f, 1e-5f);
    w.Remove<FPosition>(e);
    EXPECT_FALSE(w.Has<FPosition>(e));
}

ACS_TEST(Ecs, CanonicalComponentIdentifiersPreserveRuntimeContracts)
{
    /** 検査用コンポーネントへ割り当てられた実行時番号。 */
    const FComponentTypeId id = GetComponentTypeId<FPosition>();
    /** 型登録で得た操作情報。 */
    const FComponentOps& registered = CComponentRegistry::Register<FPosition>();
    /** 同じ番号から取り直した操作情報。 */
    const FComponentOps& fetched = CComponentRegistry::Get(id);

    EXPECT_TRUE(&registered == &fetched);
    EXPECT_EQ(id, TComponentTypeTraits<FPosition>::RuntimeId());
    EXPECT_TRUE(id < kMaxComponentTypes);
    EXPECT_EQ(GetComponentSignatureId<FPosition>(), TComponentTypeTraits<FPosition>::Signature);
}

ACS_TEST(Ecs, GenerationInvalidatesOldId) {
    CWorld w;
    FEntityId a = w.Create();
    w.Destroy(a);
    FEntityId b = w.Create();  // 同じ index を再利用するが世代が違う
    EXPECT_FALSE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
}

ACS_TEST(Ecs, ClearReleasesAllStateAndAllowsReuse)
{
    CWorld world;
    const FEntityId old_entity = world.Create();
    world.Add<FPosition>(old_entity, FPosition{1.0f, 2.0f, 3.0f});
    EXPECT_EQ(world.EntityCount(), 1u);

    world.Clear();
    world.Clear();
    EXPECT_EQ(world.EntityCount(), 0u);
    EXPECT_TRUE(!world.IsAlive(old_entity));

    const FEntityId new_entity = world.Create();
    world.Add<FPosition>(new_entity, FPosition{4.0f, 5.0f, 6.0f});
    EXPECT_TRUE(world.IsAlive(new_entity));
    EXPECT_TRUE(!world.IsAlive(old_entity));
    EXPECT_TRUE(world.Get<FPosition>(new_entity) != nullptr);
}

ACS_TEST(Ecs, QueryIteratesMatching) {
    CWorld w;
    for (int i = 0; i < 100; ++i) {
        FEntityId e = w.Create();
        w.Add<FPosition>(e, {(f32)i, 0, 0});
        if (i % 2 == 0) w.Add<FVelocity>(e, {1, 0, 0});
    }
    u32 count = 0;
    w.Query<FPosition, FVelocity>().Each([&count](FEntityId, FPosition&, FVelocity&){ ++count; });
    EXPECT_EQ(count, 50u);  // 偶数 i のみ Velocity を持つ
}

ACS_TEST(Ecs, QuerySnapshotRejectsDestroyedGenerationAndDefersReusedSlot)
{
    CWorld world;
    const FEntityId first = world.Create();
    const FEntityId stale = world.Create();
    world.Add<FPosition>(first, {1.0f, 0.0f, 0.0f});
    world.Add<FPosition>(stale, {2.0f, 0.0f, 0.0f});

    u32 visits = 0u;
    FEntityId replacement{};
    const auto mutate_during_visit = [&](FEntityId entity, FPosition&) {
        ++visits;
        if (entity == first) {
            world.Destroy(stale);
            replacement = world.Create();
            world.Add<FPosition>(replacement, {3.0f, 0.0f, 0.0f});
        }
    };
    world.Query<FPosition>().Each(mutate_during_visit);

    EXPECT_EQ(visits, 1u);
    EXPECT_FALSE(world.IsAlive(stale));
    EXPECT_TRUE(world.IsAlive(replacement));

    u32 next_visits = 0u;
    world.Query<FPosition>().Each([&](FEntityId, FPosition&) { ++next_visits; });
    EXPECT_EQ(next_visits, 2u);
}

ACS_TEST(Ecs, SystemSchedulerRuns) {
    CWorld w;
    for (int i = 0; i < 10; ++i) {
        FEntityId e = w.Create();
        w.Add<FPosition>(e, {0, 0, 0});
        w.Add<FVelocity>(e, {1, 0, 0});
    }
    CSystemScheduler s;
    s.Add([](CWorld& w, f32 dt){
        w.Query<FPosition, FVelocity>().Each([dt](FEntityId, FPosition& p, FVelocity& v){
            p.x += v.dx * dt;
        });
    });
    s.Tick(w, 1.0f);
    s.Tick(w, 1.0f);
    bool all_two = true;
    w.Query<FPosition>().Each([&all_two](FEntityId, FPosition& p){
        if (p.x != 2.0f) all_two = false;
    });
    EXPECT_TRUE(all_two);
}

ACS_TEST(Ecs, EntityCommandBufferDefersAndAppliesStructuralChanges)
{
    CWorld w;
    const FEntityId a = w.Create();
    const FEntityId b = w.Create();
    const FEntityId c = w.Create();
    w.Add<FHealth>(a, {10});
    w.Add<FHealth>(b, {0});
    w.Add<FHealth>(c, {-5});

    FEntityCommandBuffer cmd(w);
    // 反復中は構造変更せず記録だけ。渡された Health& は反復中ずっと有効 (dangling しない)。
    w.Query<FHealth>().Each([&cmd](FEntityId e, FHealth& h) {
        if (h.hp <= 0) {
            cmd.Destroy(e);                          // hp<=0 のエンティティを破棄予約
        } else {
            cmd.Add<FVelocity>(e, {1.0f, 0.0f, 0.0f}); // 生存者へ Velocity 追加予約
        }
    });

    // 反復中は何も適用されていない。
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
    EXPECT_TRUE(w.IsAlive(c));
    EXPECT_FALSE(w.Has<FVelocity>(a));
    EXPECT_EQ(cmd.Size(), static_cast<usize>(3));
    EXPECT_FALSE(cmd.HasOverflowed());

    cmd.Flush();
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

    // hp<=0 の b,c は破棄され、a は生存 + Velocity 付与。
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_FALSE(w.IsAlive(b));
    EXPECT_FALSE(w.IsAlive(c));
    EXPECT_TRUE(w.Has<FVelocity>(a));
    FVelocity* const v = w.Get<FVelocity>(a);
    EXPECT_TRUE(v != nullptr);
    if (v) EXPECT_NEAR(v->dx, 1.0f, 1e-5f);
}

ACS_TEST(Ecs, EntityCommandBufferRemoveAndClearDiscardsWithoutApplying)
{
    CWorld w;
    const FEntityId e = w.Create();
    w.Add<FPosition>(e, {1, 2, 3});
    w.Add<FVelocity>(e, {4, 5, 6});

    {
        FEntityCommandBuffer cmd(w);
        cmd.Remove<FVelocity>(e);
        cmd.Add<FHealth>(e, {99});
        // Clear は適用せず破棄する (退避した Health 値も解放される)。
        cmd.Clear();
        EXPECT_EQ(cmd.Size(), static_cast<usize>(0));
    }
    // Clear したので World は変わらない。
    EXPECT_TRUE(w.Has<FVelocity>(e));
    EXPECT_FALSE(w.Has<FHealth>(e));

    // Flush 経路の Remove も確認する。
    FEntityCommandBuffer cmd2(w);
    cmd2.Remove<FVelocity>(e);
    cmd2.Flush();
    EXPECT_FALSE(w.Has<FVelocity>(e));
    EXPECT_TRUE(w.Has<FPosition>(e));
}

ACS_TEST(Ecs, EntityCommandBufferGracefullyHandlesOutOfMemory)
{
    CWorld w;
    const FEntityId e = w.Create();

    // command buffer 自身のストレージを常時失敗 backing に載せると、記録は落ちるが
    // クラッシュせず HasOverflowed() で検知でき、Flush も安全 (何も適用しない)。
    FEcbFailAllocator failing;
    FEntityCommandBuffer cmd(w, failing);
    cmd.Destroy(e);              // TryPushBack が失敗
    cmd.Add<FHealth>(e, {5});     // New が失敗 (値の退避不可)
    cmd.Remove<FHealth>(e);       // TryPushBack が失敗

    EXPECT_TRUE(cmd.HasOverflowed());
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

    cmd.Flush();                 // 記録ゼロなので no-op、クラッシュしない
    EXPECT_TRUE(w.IsAlive(e));    // Destroy は落ちたのでエンティティは生存
}

ACS_TEST(Ecs, EntityCommandBufferInlinesSmallValuesAfterBatchReserve)
{
    CWorld world;
    CSystemAllocator allocator;
    FEntityCommandBuffer commands(world, allocator);
    constexpr usize kCount = 128u;
    EXPECT_TRUE(commands.TryReserve(kCount));
    const u64 reserved_allocations = allocator.AllocationCount();
    EXPECT_EQ(reserved_allocations, 1ull);

    FEntityId entities[kCount]{};
    for (usize i = 0; i < kCount; ++i) {
        entities[i] = world.Create();
        commands.Add<FHealth>(entities[i], FHealth{static_cast<i32>(i)});
    }
    EXPECT_EQ(allocator.AllocationCount(), reserved_allocations);
    EXPECT_FALSE(commands.HasOverflowed());

    commands.Flush();
    EXPECT_EQ(allocator.AllocationCount(), reserved_allocations);
    EXPECT_EQ(world.Get<FHealth>(entities[kCount - 1u])->hp, static_cast<i32>(kCount - 1u));
}

ACS_TEST(Ecs, WorldCopyFromSnapshotAndRollback)
{
    // rollback netcode の要件: snapshot 時の EntityId は復元後も有効、snapshot 後に
    // 作った EntityId は復元で無効、値・コンポーネント構成・生存状態が完全に巻き戻る。
    CWorld w;
    const FEntityId a = w.Create();
    w.Add<FPosition>(a, {1, 2, 3});
    w.Add<FHealth>(a, {10});
    const FEntityId b = w.Create();
    w.Add<FPosition>(b, {4, 5, 6});

    CWorld snap;
    EXPECT_TRUE(snap.CopyFrom(w));
    EXPECT_EQ(snap.EntityCount(), 2u);

    // snapshot は独立コピー: 元 World の変更が snapshot に波及しない。
    w.Get<FPosition>(a)->x = 100.0f;
    w.Destroy(b);
    const FEntityId c = w.Create();       // b のスロット再利用 (世代は進んでいる)
    w.Add<FHealth>(c, {77});
    EXPECT_NEAR(snap.Get<FPosition>(a)->x, 1.0f, 1e-5f);

    // rollback: フレーム N の状態へ完全に巻き戻す。
    EXPECT_TRUE(w.CopyFrom(snap));
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));           // snapshot 時点で生存 → 復活
    EXPECT_FALSE(w.IsAlive(c));          // snapshot 後に作った id は世代不一致で無効
    EXPECT_EQ(w.EntityCount(), 2u);
    EXPECT_NEAR(w.Get<FPosition>(a)->x, 1.0f, 1e-5f);   // 値も巻き戻る
    FHealth* const ha = w.Get<FHealth>(a);
    EXPECT_TRUE(ha != nullptr);
    if (ha) EXPECT_EQ(ha->hp, 10);
    EXPECT_TRUE(w.Has<FPosition>(b));
    EXPECT_FALSE(w.Has<FHealth>(b));

    // 復元後も通常運用できる (Create / Add / Destroy)。
    const FEntityId d = w.Create();
    w.Add<FVelocity>(d, {1, 0, 0});
    EXPECT_TRUE(w.IsAlive(d));
    EXPECT_EQ(w.EntityCount(), 3u);

    // 自己コピーは no-op で成功する。
    EXPECT_TRUE(w.CopyFrom(w));
    EXPECT_EQ(w.EntityCount(), 3u);
}

ACS_TEST(Ecs, WorldCopyFromRejectsNonCopyableComponents)
{
    // 非コピー型の SparseSet を持つ World は複製できず、部分複製も残さない。
    CWorld w;
    const FEntityId e = w.Create();
    w.Add<FPosition>(e, {1, 2, 3});
    w.Add<FMoveOnlyComp>(e, FMoveOnlyComp{5});

    CWorld snap;
    EXPECT_FALSE(snap.CopyFrom(w));
    EXPECT_EQ(snap.EntityCount(), 0u);   // 失敗時は空 (Clear 済み) に戻る
    EXPECT_FALSE(snap.IsAlive(e));

    // 元 World は無傷。
    EXPECT_TRUE(w.IsAlive(e));
    EXPECT_TRUE(w.Get<FMoveOnlyComp>(e) != nullptr);
}

ACS_TEST(Ecs, EntityCommandBufferDeferredCreate)
{
    CWorld w;
    FEntityCommandBuffer cmd(w);
    cmd.Create();
    cmd.CreateWith<FHealth>(FHealth{42});
    EXPECT_EQ(w.EntityCount(), 0u);      // Flush まで生成されない
    EXPECT_EQ(cmd.Size(), static_cast<usize>(2));

    cmd.Flush();
    EXPECT_EQ(w.EntityCount(), 2u);
    u32 with_health = 0;
    i32 hp = 0;
    w.Query<FHealth>().Each([&](FEntityId, FHealth& h) { ++with_health; hp = h.hp; });
    EXPECT_EQ(with_health, 1u);
    EXPECT_EQ(hp, 42);

    // Clear は生成せず退避値も解放する (リークは CRT/ASan 検査で担保)。
    cmd.CreateWith<FHealth>(FHealth{7});
    cmd.Clear();
    cmd.Flush();
    EXPECT_EQ(w.EntityCount(), 2u);
}

ACS_TEST(Ecs, ParallelCommandBufferRecordsFromWorkersAndApplies)
{
    // EachParallel の fn 内から FParallelEntityCommandBuffer へロックなしで記録し、
    // 完了後の Flush で一括適用できることを検証する (per-worker スロット分離の実地確認)。
    EXPECT_TRUE(CThreadPool::Init(4).IsOk());
    {
        CWorld w;
        constexpr u32 kCount = 2000u;
        for (u32 i = 0; i < kCount; ++i) {
            const FEntityId e = w.Create();
            w.Add<FHealth>(e, {static_cast<i32>(i % 3u)});   // hp: 0,1,2,0,1,2,...
        }

        FParallelEntityCommandBuffer cmd(w);
        EXPECT_TRUE(cmd.IsValid());

        // grain を小さくして複数ワーカー + 呼び出し元 (Wait 中の steal) に分散させる。
        w.Query<FHealth>().EachParallel([&cmd](FEntityId e, FHealth& h) {
            if (h.hp <= 0) {
                cmd.Destroy(e);                              // hp==0 (1/3) を破棄予約
            } else {
                cmd.Add<FVelocity>(e, {1.0f, 0.0f, 0.0f});    // 残り 2/3 へ Velocity 追加予約
            }
        }, 64u);

        // EachParallel 中は何も適用されない。
        EXPECT_EQ(w.EntityCount(), kCount);
        EXPECT_EQ(cmd.Size(), static_cast<usize>(kCount));
        EXPECT_FALSE(cmd.HasOverflowed());

        cmd.Flush();
        EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

        // hp==0 は 667 体 (i%3==0: 0,3,...,1998)、残り 1333 体に Velocity。
        constexpr u32 kDestroyed = (kCount + 2u) / 3u;
        EXPECT_EQ(w.EntityCount(), kCount - kDestroyed);
        u32 with_velocity = 0;
        w.Query<FVelocity>().Each([&with_velocity](FEntityId, FVelocity&) { ++with_velocity; });
        EXPECT_EQ(with_velocity, kCount - kDestroyed);
    }
    CThreadPool::Shutdown();
}

ACS_TEST(Ecs, ParallelCommandBufferDeferredCreateSpawnsAfterFlush)
{
    // EachParallel 中の生成は World::Create がスレッドセーフでないため CreateWith で
    // 遅延記録し、Flush で一括生成する (並列スポーンの実地確認)。
    EXPECT_TRUE(CThreadPool::Init(4).IsOk());
    {
        CWorld w;
        constexpr u32 kCount = 500u;
        for (u32 i = 0; i < kCount; ++i) {
            const FEntityId e = w.Create();
            w.Add<FPosition>(e, {static_cast<f32>(i), 0, 0});
        }

        FParallelEntityCommandBuffer cmd(w);
        w.Query<FPosition>().EachParallel([&cmd](FEntityId, FPosition& p) {
            if ((static_cast<u32>(p.x) % 2u) == 0u) {
                cmd.CreateWith<FHealth>(FHealth{static_cast<i32>(p.x)});
            }
        }, 32u);

        EXPECT_EQ(w.EntityCount(), kCount);            // Flush まで生成されない
        EXPECT_FALSE(cmd.HasOverflowed());
        cmd.Flush();
        EXPECT_EQ(w.EntityCount(), kCount + kCount / 2u);
        u32 spawned = 0;
        w.Query<FHealth>().Each([&spawned](FEntityId, FHealth&) { ++spawned; });
        EXPECT_EQ(spawned, kCount / 2u);
    }
    CThreadPool::Shutdown();
}

ACS_TEST(Ecs, ParallelCommandBufferWorksWithoutThreadPool)
{
    // プール未初期化でも構築できる (スロット = 非ワーカー用の 1 本)。逐次 Each からの
    // 記録・Flush が単体の FEntityCommandBuffer と同じに動くことを確認する。
    CWorld w;
    const FEntityId a = w.Create();
    const FEntityId b = w.Create();
    w.Add<FHealth>(a, {5});
    w.Add<FHealth>(b, {0});

    FParallelEntityCommandBuffer cmd(w);
    EXPECT_TRUE(cmd.IsValid());
    w.Query<FHealth>().Each([&cmd](FEntityId e, FHealth& h) {
        if (h.hp <= 0) cmd.Destroy(e);
        else           cmd.Add<FVelocity>(e, {2.0f, 0.0f, 0.0f});
    });
    EXPECT_EQ(cmd.Size(), static_cast<usize>(2));
    cmd.Flush();

    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_FALSE(w.IsAlive(b));
    EXPECT_TRUE(w.Has<FVelocity>(a));

    // Clear は適用せず破棄する (Add の退避値もリークしない — ASan/CRT リーク検査で担保)。
    cmd.Add<FHealth>(a, {99});
    EXPECT_EQ(cmd.Size(), static_cast<usize>(1));
    cmd.Clear();
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));
    EXPECT_EQ(w.Get<FHealth>(a)->hp, 5);
}

ACS_TEST(Ecs, QueryEachExcludingSkipsEntitiesWithExcludedComponents)
{
    CWorld w;
    for (int i = 0; i < 20; ++i) {
        const FEntityId e = w.Create();
        w.Add<FPosition>(e, {static_cast<f32>(i), 0, 0});
        if (i % 5 == 0) w.Add<FFrozen>(e, {0});  // 0,5,10,15 を Frozen に
    }

    u32 visited = 0;
    u32 frozen_visited = 0;
    w.Query<FPosition>().EachExcluding<FFrozen>([&](FEntityId e, FPosition&) {
        ++visited;
        if (w.Has<FFrozen>(e)) ++frozen_visited;  // 除外されるので 0 のはず
    });
    EXPECT_EQ(visited, 16u);        // 20 - 4 (Frozen) = 16
    EXPECT_EQ(frozen_visited, 0u);  // Frozen は 1 つも訪問しない

    // 空の Excludes は Each と同じ (全 Position を訪問)。
    u32 all = 0;
    w.Query<FPosition>().EachExcluding<>([&](FEntityId, FPosition&) { ++all; });
    EXPECT_EQ(all, 20u);
}

ACS_TEST(Ecs, QueryOptionalSpecializationReturnsNullablePointers)
{
    CWorld world;
    const FEntityId with_optional = world.Create();
    const FEntityId without_optional = world.Create();
    world.Add<FPosition>(with_optional, {1.0f, 0.0f, 0.0f});
    world.Add<FPosition>(without_optional, {2.0f, 0.0f, 0.0f});
    world.Add<FVelocity>(with_optional, {3.0f, 0.0f, 0.0f});

    u32 present = 0u;
    u32 missing = 0u;
    const auto visit_optional = [&](FEntityId, FPosition&, FVelocity* velocity) {
        if (velocity != nullptr) ++present;
        else ++missing;
    };
    world.Query<FPosition>().EachOptional<FVelocity>(visit_optional);
    EXPECT_EQ(present, 1u);
    EXPECT_EQ(missing, 1u);
}
