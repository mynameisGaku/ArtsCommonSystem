// SPDX-License-Identifier: Apache-2.0
// CCollisionWorld3Dの世代付きhandle、layer/exclude、AABB/Sphere query契約を検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/CollisionWorld3D.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** shape列挙結果に指定handleが含まれるか返す。 */
bool ContainsShape(const TArray<FCollisionShapeId3D>& shapes, FCollisionShapeId3D id) noexcept
{
    for (u32 index = 0u; index < shapes.Num(); ++index) {
        if (shapes[index] == id) return true;
    }
    return false;
}

} // namespace

ACS_TEST(CollisionWorld3D, ReusesSlotsWithoutRevivingStaleHandles)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D first = world.TryAddAabb(FAabb3{FVec3{0.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}});
    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(world.IsAlive(first));
    EXPECT_EQ(world.ShapeCount(), 1u);
    EXPECT_TRUE(world.TryRemove(first));
    EXPECT_FALSE(world.TryRemove(first));
    EXPECT_FALSE(world.IsAlive(first));

    const FCollisionShapeId3D reused = world.TryAddSphere(FSphere{FVec3{2.0f, 0.0f, 0.0f}, 1.0f});
    EXPECT_TRUE(reused.IsValid());
    EXPECT_EQ(reused.Index(), first.Index());
    EXPECT_TRUE(reused.Generation() != first.Generation());
    EXPECT_FALSE(world.IsAlive(first));
    EXPECT_TRUE(world.IsAlive(reused));

    world.ClearAll();
    EXPECT_EQ(world.ShapeCount(), 0u);
    EXPECT_FALSE(world.IsAlive(reused));
    const FCollisionShapeId3D after_clear = world.TryAddAabb(FAabb3{FVec3{}, FVec3{0.5f, 0.5f, 0.5f}});
    EXPECT_EQ(after_clear.Index(), reused.Index());
    EXPECT_TRUE(after_clear.Generation() != reused.Generation());
    EXPECT_FALSE(world.IsAlive(reused));
}

ACS_TEST(CollisionWorld3D, RejectsInvalidShapesAndWrongKindUpdates)
{
    CCollisionWorld3D world;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(world.TryAddAabb(FAabb3{FVec3{nan, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}).IsValid());
    EXPECT_FALSE(world.TryAddAabb(FAabb3{FVec3{}, FVec3{-1.0f, 1.0f, 1.0f}}).IsValid());
    EXPECT_FALSE(world.TryAddSphere(FSphere{FVec3{}, 0.0f}).IsValid());
    EXPECT_FALSE(world.TryAddSphere(FSphere{FVec3{}, nan}).IsValid());
    EXPECT_EQ(world.ShapeCount(), 0u);

    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}}, 0x4u);
    EXPECT_TRUE(box.IsValid());
    EXPECT_FALSE(world.TryUpdateSphere(box, FSphere{FVec3{}, 1.0f}));
    EXPECT_FALSE(world.TryUpdateAabb(box, FAabb3{FVec3{}, FVec3{1.0f, -1.0f, 1.0f}}));
    u32 layer = 99u;
    EXPECT_TRUE(world.TryGetLayer(box, layer));
    EXPECT_EQ(layer, 0x4u);
    EXPECT_TRUE(world.TrySetLayer(box, 0x8u));
    EXPECT_TRUE(world.TryGetLayer(box, layer));
    EXPECT_EQ(layer, 0x8u);
}

ACS_TEST(CollisionWorld3D, OverlapUsesLayersExactExcludeAndStableOrder)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{0.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}, 0x1u);
    const FCollisionShapeId3D sphere = world.TryAddSphere(FSphere{FVec3{1.5f, 0.0f, 0.0f}, 1.0f}, 0x2u);
    const FCollisionShapeId3D distant = world.TryAddAabb(FAabb3{FVec3{10.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}, 0x1u);
    EXPECT_TRUE(box.IsValid() && sphere.IsValid() && distant.IsValid());

    TArray<FCollisionShapeId3D> hits;
    EXPECT_TRUE(world.TryOverlapSphere(FSphere{FVec3{0.5f, 0.0f, 0.0f}, 2.0f}, hits));
    EXPECT_EQ(hits.Num(), 2u);
    EXPECT_TRUE(hits[0u] == box);
    EXPECT_TRUE(hits[1u] == sphere);
    EXPECT_FALSE(ContainsShape(hits, distant));

    EXPECT_TRUE(world.TryOverlapAabb(FAabb3{FVec3{0.5f, 0.0f, 0.0f}, FVec3{2.0f, 2.0f, 2.0f}}, hits, box, 0x1u));
    EXPECT_EQ(hits.Num(), 0u);
    EXPECT_TRUE(world.TryOverlapAabb(FAabb3{FVec3{0.5f, 0.0f, 0.0f}, FVec3{2.0f, 2.0f, 2.0f}}, hits, {}, 0x2u));
    EXPECT_EQ(hits.Num(), 1u);
    EXPECT_TRUE(hits[0u] == sphere);

    EXPECT_TRUE(world.TryRemove(box));
    const FCollisionShapeId3D replacement = world.TryAddAabb(FAabb3{FVec3{0.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}, 0x1u);
    EXPECT_EQ(replacement.Index(), box.Index());
    EXPECT_TRUE(replacement.Generation() != box.Generation());
    EXPECT_TRUE(world.TryOverlapAabb(FAabb3{FVec3{0.0f, 0.0f, 0.0f}, FVec3{2.0f, 2.0f, 2.0f}}, hits, box, 0x1u));
    EXPECT_EQ(hits.Num(), 1u);
    EXPECT_TRUE(hits[0u] == replacement);
}

ACS_TEST(CollisionWorld3D, OverlapFailurePreservesOutput)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D shape = world.TryAddSphere(FSphere{FVec3{}, 1.0f});
    TArray<FCollisionShapeId3D> hits;
    EXPECT_TRUE(hits.TryAdd(shape));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(world.TryOverlapAabb(FAabb3{FVec3{}, FVec3{nan, 1.0f, 1.0f}}, hits));
    EXPECT_EQ(hits.Num(), 1u);
    EXPECT_TRUE(hits[0u] == shape);
    EXPECT_FALSE(world.TryOverlapSphere(FSphere{FVec3{}, -1.0f}, hits));
    EXPECT_EQ(hits.Num(), 1u);
    EXPECT_TRUE(hits[0u] == shape);
}

ACS_TEST(CollisionWorld3D, RaycastReturnsNearestWorldHitAndHonorsFilters)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D sphere = world.TryAddSphere(FSphere{FVec3{3.0f, 0.0f, 0.0f}, 1.0f}, 0x1u);
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{6.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}, 0x2u);
    const FRay3 ray{FVec3{}, FVec3{2.0f, 0.0f, 0.0f}};
    FRayHit3 hit;
    FCollisionShapeId3D hit_shape;
    EXPECT_TRUE(world.TryRaycast(ray, 0.0f, 10.0f, hit, hit_shape));
    EXPECT_TRUE(hit_shape == sphere);
    EXPECT_NEAR(hit.t, 1.0f, 1.0e-6f);
    EXPECT_NEAR(hit.point.x, 2.0f, 1.0e-6f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TryRaycast(ray, 0.0f, 10.0f, hit, hit_shape, sphere));
    EXPECT_TRUE(hit_shape == box);
    EXPECT_NEAR(hit.t, 2.5f, 1.0e-6f);
    EXPECT_TRUE(world.TryRaycast(ray, 0.0f, 10.0f, hit, hit_shape, {}, 0x2u));
    EXPECT_TRUE(hit_shape == box);
    EXPECT_TRUE(world.TryRaycast(ray, 1.5f, 10.0f, hit, hit_shape));
    EXPECT_TRUE(hit_shape == box);
}

ACS_TEST(CollisionWorld3D, RaycastMissAndInvalidInputPreserveOutputs)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D sphere = world.TryAddSphere(FSphere{FVec3{}, 2.0f});
    const FRayHit3 preserved_hit{true, 17.0f, FVec3{1.0f, 2.0f, 3.0f}, FVec3{0.0f, 1.0f, 0.0f}};
    FRayHit3 hit = preserved_hit;
    FCollisionShapeId3D hit_shape = sphere;
    EXPECT_FALSE(world.TryRaycast(FRay3{FVec3{5.0f, 0.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f}}, 0.0f, 10.0f, hit, hit_shape));
    EXPECT_EQ(hit.t, preserved_hit.t);
    EXPECT_TRUE(hit_shape == sphere);
    EXPECT_FALSE(world.TryRaycast(FRay3{FVec3{}, FVec3{}}, 0.0f, 10.0f, hit, hit_shape));
    EXPECT_FALSE(world.TryRaycast(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, 2.0f, 1.0f, hit, hit_shape));
    EXPECT_EQ(hit.t, preserved_hit.t);
    EXPECT_TRUE(hit_shape == sphere);

    EXPECT_TRUE(world.TryRaycast(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, 0.0f, 1.0f, hit, hit_shape));
    EXPECT_TRUE(hit_shape == sphere);
    EXPECT_EQ(hit.t, 0.0f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1.0e-6f);
}
