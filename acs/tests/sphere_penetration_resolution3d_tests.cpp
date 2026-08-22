// SPDX-License-Identifier: Apache-2.0
// 3D sphere反復貫通解消adapterの決定性、反復上限、失敗時不変条件を検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SpherePenetrationResolution3D.h"

#include <limits>

using namespace acs;
using namespace acs::game;

ACS_TEST(SpherePenetrationResolution3D, LeavesSeparatedSphereUnchanged)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(world.TryAddAabb(FAabb3{FVec3{5.0f, 0.0f, 0.0f}, FVec3{1.0f, 1.0f, 1.0f}}).IsValid());
    const FSphere sphere{FVec3{}, 0.5f};
    FSpherePenetrationResolution3D result;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, sphere, result));
    EXPECT_TRUE(result.FullyResolved);
    EXPECT_EQ(result.IterationCount, 0u);
    EXPECT_NEAR(result.ResolvedSphere.center.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.ResolvedSphere.radius, 0.5f, 1.0e-6f);
    EXPECT_NEAR(result.Translation.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.Translation.y, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.Translation.z, 0.0f, 1.0e-6f);
}

ACS_TEST(SpherePenetrationResolution3D, ResolvesSingleAabbFaceWithoutMutatingWorld)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D box = world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}});
    FSpherePenetrationResolution3D result;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, FSphere{FVec3{1.25f, 0.0f, 0.0f}, 0.5f}, result));
    EXPECT_TRUE(result.FullyResolved);
    EXPECT_EQ(result.IterationCount, 1u);
    EXPECT_NEAR(result.ResolvedSphere.center.x, 1.5f, 1.0e-6f);
    EXPECT_NEAR(result.Translation.x, 0.25f, 1.0e-6f);
    EXPECT_TRUE(world.IsAlive(box));
    FCollisionPenetration3D original_penetration;
    EXPECT_TRUE(world.TryFindSpherePenetration(FSphere{FVec3{1.25f, 0.0f, 0.0f}, 0.5f}, original_penetration));
}

ACS_TEST(SpherePenetrationResolution3D, ResolvesEqualDepthContactsInStableSlotOrder)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(world.TryAddAabb(FAabb3{FVec3{-0.75f, 0.0f, 0.0f}, FVec3{0.5f, 2.0f, 2.0f}}).IsValid());
    EXPECT_TRUE(world.TryAddAabb(FAabb3{FVec3{0.0f, -0.75f, 0.0f}, FVec3{2.0f, 0.5f, 2.0f}}).IsValid());
    const FSphere sphere{FVec3{}, 1.0f};

    FSpherePenetrationResolution3D limited;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, sphere, limited, 1u));
    EXPECT_FALSE(limited.FullyResolved);
    EXPECT_EQ(limited.IterationCount, 1u);
    EXPECT_NEAR(limited.ResolvedSphere.center.x, 0.75f, 1.0e-6f);
    EXPECT_NEAR(limited.ResolvedSphere.center.y, 0.0f, 1.0e-6f);

    FSpherePenetrationResolution3D complete;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, sphere, complete, 2u));
    EXPECT_TRUE(complete.FullyResolved);
    EXPECT_EQ(complete.IterationCount, 2u);
    EXPECT_NEAR(complete.ResolvedSphere.center.x, 0.75f, 1.0e-6f);
    EXPECT_NEAR(complete.ResolvedSphere.center.y, 0.75f, 1.0e-6f);
    EXPECT_NEAR(complete.Translation.x, 0.75f, 1.0e-6f);
    EXPECT_NEAR(complete.Translation.y, 0.75f, 1.0e-6f);
}

ACS_TEST(SpherePenetrationResolution3D, ZeroIterationsOnlyReportsConvergence)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(world.TryAddSphere(FSphere{FVec3{}, 1.0f}).IsValid());
    FSpherePenetrationResolution3D result;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, FSphere{FVec3{1.5f, 0.0f, 0.0f}, 1.0f}, result, 0u));
    EXPECT_FALSE(result.FullyResolved);
    EXPECT_EQ(result.IterationCount, 0u);
    EXPECT_NEAR(result.ResolvedSphere.center.x, 1.5f, 1.0e-6f);
    EXPECT_NEAR(result.Translation.x, 0.0f, 1.0e-6f);
}

ACS_TEST(SpherePenetrationResolution3D, HonorsLayerAndExactExclude)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D first = world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}}, 0x1u);
    const FCollisionShapeId3D second = world.TryAddSphere(FSphere{FVec3{2.0f, 0.0f, 0.0f}, 1.0f}, 0x2u);
    const FSphere sphere{FVec3{1.25f, 0.0f, 0.0f}, 0.5f};
    FSpherePenetrationResolution3D result;
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, sphere, result, 4u, first, 0x3u));
    EXPECT_TRUE(result.FullyResolved);
    EXPECT_NEAR(result.Translation.x, -0.75f, 1.0e-6f);

    EXPECT_TRUE(world.TryRemove(first));
    EXPECT_TRUE(world.TryAddAabb(FAabb3{FVec3{}, FVec3{1.0f, 1.0f, 1.0f}}, 0x1u).IsValid());
    EXPECT_TRUE(TryResolveSpherePenetrations3D(world, sphere, result, 4u, first, 0x1u));
    EXPECT_TRUE(result.FullyResolved);
    EXPECT_NEAR(result.Translation.x, 0.25f, 1.0e-6f);
    EXPECT_TRUE(world.IsAlive(second));
}

ACS_TEST(SpherePenetrationResolution3D, InvalidInputPreservesOutput)
{
    CCollisionWorld3D world;
    const FSpherePenetrationResolution3D preserved{FSphere{FVec3{1.0f, 2.0f, 3.0f}, 4.0f}, FVec3{5.0f, 6.0f, 7.0f}, 8u, true};
    FSpherePenetrationResolution3D result = preserved;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(TryResolveSpherePenetrations3D(world, FSphere{FVec3{}, 0.0f}, result));
    EXPECT_FALSE(TryResolveSpherePenetrations3D(world, FSphere{FVec3{nan, 0.0f, 0.0f}, 1.0f}, result));
    EXPECT_FALSE(TryResolveSpherePenetrations3D(world, FSphere{FVec3{}, 1.0f}, result, FSpherePenetrationResolution3D::kMaximumIterations + 1u));
    EXPECT_NEAR(result.ResolvedSphere.center.x, preserved.ResolvedSphere.center.x, 1.0e-6f);
    EXPECT_NEAR(result.ResolvedSphere.radius, preserved.ResolvedSphere.radius, 1.0e-6f);
    EXPECT_NEAR(result.Translation.x, preserved.Translation.x, 1.0e-6f);
    EXPECT_EQ(result.IterationCount, preserved.IterationCount);
    EXPECT_EQ(result.FullyResolved, preserved.FullyResolved);
}
