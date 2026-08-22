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

ACS_TEST(CollisionWorld3D, SphereSweepUsesExactRoundedAabbEdges)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{5.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}});
    FCollisionSweepHit3D hit;
    EXPECT_TRUE(world.TrySweepSphere(FRay3{FVec3{0.0f, 1.5f, 0.0f}, FVec3{2.0f, 0.0f, 0.0f}}, 0.5f, 0.0f, 10.0f, hit));
    EXPECT_TRUE(hit.IsValid());
    EXPECT_TRUE(hit.Shape == box);
    EXPECT_FALSE(hit.StartedOverlapping);
    EXPECT_NEAR(hit.T, 2.0f, 1.0e-6f);
    EXPECT_NEAR(hit.Center.x, 4.0f, 1.0e-6f);
    EXPECT_NEAR(hit.Center.y, 1.5f, 1.0e-6f);
    EXPECT_NEAR(hit.Normal.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(hit.Normal.y, 1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{2.0f, 0.0f, 0.0f}}, 0.0f, 0.0f, 10.0f, hit));
    EXPECT_NEAR(hit.T, 2.0f, 1.0e-6f);
    EXPECT_NEAR(hit.Center.x, 4.0f, 1.0e-6f);
    EXPECT_NEAR(hit.Normal.x, -1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TrySweepSphere(FRay3{FVec3{0.0f, 5.0f, 5.0f}, FVec3{1.0f, -1.0f, -1.0f}}, 1.0f, 0.0f, 10.0f, hit));
    EXPECT_NEAR(hit.T, 3.4226497f, 1.0e-5f);
    EXPECT_NEAR(hit.Normal.x, -0.5773503f, 1.0e-5f);
    EXPECT_NEAR(hit.Normal.y, 0.5773503f, 1.0e-5f);
    EXPECT_NEAR(hit.Normal.z, 0.5773503f, 1.0e-5f);
}

ACS_TEST(CollisionWorld3D, SphereSweepReturnsNearestShapeAndHonorsFilters)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D sphere = world.TryAddSphere(FSphere{FVec3{3.0f, 0.0f, 0.0f}, 1.0f}, 0x1u);
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{6.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}, 0x2u);
    const FRay3 center_ray{FVec3{}, FVec3{2.0f, 0.0f, 0.0f}};
    FCollisionSweepHit3D hit;
    EXPECT_TRUE(world.TrySweepSphere(center_ray, 0.5f, 0.0f, 10.0f, hit));
    EXPECT_TRUE(hit.Shape == sphere);
    EXPECT_NEAR(hit.T, 0.75f, 1.0e-6f);
    EXPECT_NEAR(hit.Center.x, 1.5f, 1.0e-6f);
    EXPECT_NEAR(hit.Normal.x, -1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TrySweepSphere(center_ray, 0.5f, 0.0f, 10.0f, hit, sphere));
    EXPECT_TRUE(hit.Shape == box);
    EXPECT_NEAR(hit.T, 2.25f, 1.0e-6f);
    EXPECT_TRUE(world.TrySweepSphere(center_ray, 0.5f, 0.0f, 10.0f, hit, {}, 0x2u));
    EXPECT_TRUE(hit.Shape == box);
    EXPECT_TRUE(world.TrySweepSphere(center_ray, 0.5f, 1.0f, 10.0f, hit));
    EXPECT_TRUE(hit.Shape == box);

    EXPECT_TRUE(world.TryRemove(sphere));
    const FCollisionShapeId3D replacement = world.TryAddSphere(FSphere{FVec3{2.0f, 0.0f, 0.0f}, 1.0f}, 0x1u);
    EXPECT_EQ(replacement.Index(), sphere.Index());
    EXPECT_TRUE(replacement.Generation() != sphere.Generation());
    EXPECT_TRUE(world.TrySweepSphere(center_ray, 0.5f, 0.0f, 10.0f, hit, sphere, 0x1u));
    EXPECT_TRUE(hit.Shape == replacement);
    EXPECT_NEAR(hit.T, 0.25f, 1.0e-6f);
}

ACS_TEST(CollisionWorld3D, SphereSweepReportsInitialOverlapAndPreservesOutputOnFailure)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}});
    FCollisionSweepHit3D hit;
    EXPECT_TRUE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, 0.25f, 0.0f, 10.0f, hit));
    EXPECT_TRUE(hit.Shape == box);
    EXPECT_TRUE(hit.StartedOverlapping);
    EXPECT_EQ(hit.T, 0.0f);
    EXPECT_NEAR(hit.Normal.x, -1.0f, 1.0e-6f);

    const FCollisionSweepHit3D preserved = hit;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, -0.1f, 0.0f, 10.0f, hit));
    EXPECT_FALSE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, nan, 0.0f, 10.0f, hit));
    EXPECT_FALSE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{}}, 0.25f, 0.0f, 10.0f, hit));
    EXPECT_FALSE(world.TrySweepSphere(FRay3{FVec3{}, FVec3{1.0f, 0.0f, 0.0f}}, 0.25f, 0.1f, 10.0f, hit));
    EXPECT_FALSE(world.TrySweepSphere(FRay3{FVec3{5.0f, 0.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f}}, 0.25f, 0.0f, 10.0f, hit));
    EXPECT_TRUE(hit.Shape == preserved.Shape);
    EXPECT_EQ(hit.T, preserved.T);
    EXPECT_EQ(hit.StartedOverlapping, preserved.StartedOverlapping);
}

ACS_TEST(CollisionWorld3D, SpherePenetrationReturnsExactAabbSeparation)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}});
    FCollisionPenetration3D penetration;
    EXPECT_TRUE(world.TryFindSpherePenetration(FSphere{FVec3{1.25f, 0.0f, 0.0f}, 0.5f}, penetration));
    EXPECT_TRUE(penetration.IsValid());
    EXPECT_TRUE(penetration.Shape == box);
    EXPECT_NEAR(penetration.Depth, 0.25f, 1.0e-6f);
    EXPECT_NEAR(penetration.Normal.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(penetration.Translation().x, 0.25f, 1.0e-6f);

    EXPECT_TRUE(world.TryFindSpherePenetration(FSphere{FVec3{}, 0.5f}, penetration));
    EXPECT_NEAR(penetration.Depth, 1.5f, 1.0e-6f);
    EXPECT_NEAR(penetration.Normal.x, -1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TryFindSpherePenetration(FSphere{FVec3{1.3f, 1.4f, 0.0f}, 0.6f}, penetration));
    EXPECT_NEAR(penetration.Depth, 0.1f, 1.0e-5f);
    EXPECT_NEAR(penetration.Normal.x, 0.6f, 1.0e-5f);
    EXPECT_NEAR(penetration.Normal.y, 0.8f, 1.0e-5f);
}

ACS_TEST(CollisionWorld3D, SpherePenetrationChoosesDeepestAndHonorsFilters)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D positive_sphere = world.TryAddSphere(FSphere{FVec3{1.5f, 0.0f, 0.0f}, 1.0f}, 0x1u);
    const FCollisionShapeId3D negative_sphere = world.TryAddSphere(FSphere{FVec3{-1.5f, 0.0f, 0.0f}, 1.0f}, 0x2u);
    const FCollisionShapeId3D deep_box = world.TryAddAabb(FAabb3{FVec3{}, FVec3{0.25f, 0.25f, 0.25f}}, 0x4u);
    const FSphere query{FVec3{}, 1.0f};
    FCollisionPenetration3D penetration;
    EXPECT_TRUE(world.TryFindSpherePenetration(query, penetration, {}, 0x3u));
    EXPECT_TRUE(penetration.Shape == positive_sphere);
    EXPECT_NEAR(penetration.Depth, 0.5f, 1.0e-6f);
    EXPECT_NEAR(penetration.Normal.x, -1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TryFindSpherePenetration(query, penetration));
    EXPECT_TRUE(penetration.Shape == deep_box);
    EXPECT_NEAR(penetration.Depth, 1.25f, 1.0e-6f);
    EXPECT_TRUE(world.TryFindSpherePenetration(query, penetration, positive_sphere, 0x3u));
    EXPECT_TRUE(penetration.Shape == negative_sphere);
    EXPECT_NEAR(penetration.Normal.x, 1.0f, 1.0e-6f);

    EXPECT_TRUE(world.TryRemove(positive_sphere));
    const FCollisionShapeId3D replacement = world.TryAddSphere(FSphere{FVec3{0.5f, 0.0f, 0.0f}, 1.0f}, 0x1u);
    EXPECT_EQ(replacement.Index(), positive_sphere.Index());
    EXPECT_TRUE(replacement.Generation() != positive_sphere.Generation());
    EXPECT_TRUE(world.TryFindSpherePenetration(query, penetration, positive_sphere, 0x1u));
    EXPECT_TRUE(penetration.Shape == replacement);
    EXPECT_NEAR(penetration.Depth, 1.5f, 1.0e-6f);
}

ACS_TEST(CollisionWorld3D, SpherePenetrationRejectsTouchingAndPreservesOutput)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D shape = world.TryAddSphere(FSphere{FVec3{2.0f, 0.0f, 0.0f}, 1.0f});
    const FCollisionPenetration3D preserved{shape, 17.0f, FVec3{0.0f, 1.0f, 0.0f}};
    FCollisionPenetration3D penetration = preserved;
    EXPECT_FALSE(world.TryFindSpherePenetration(FSphere{FVec3{}, 1.0f}, penetration));
    EXPECT_FALSE(world.TryFindSpherePenetration(FSphere{FVec3{}, 0.0f}, penetration));
    EXPECT_FALSE(world.TryFindSpherePenetration(FSphere{FVec3{std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f}, 1.0f}, penetration));
    EXPECT_TRUE(penetration.Shape == preserved.Shape);
    EXPECT_EQ(penetration.Depth, preserved.Depth);
    EXPECT_NEAR(penetration.Normal.y, preserved.Normal.y, 1.0e-6f);
}
