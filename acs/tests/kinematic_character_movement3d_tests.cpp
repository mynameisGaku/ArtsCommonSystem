// SPDX-License-Identifier: Apache-2.0
// sphere型kinematic character移動の接地、slide、filter、失敗時不変性を検証する。
#include "gameframework/collision/KinematicCharacterMovement3D.h"
#include "memory/Memory.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <limits>
#include <type_traits>

using namespace acs;
using namespace acs::game;

namespace {

/** vectorの3成分を指定誤差内で比較する。 */
void ExpectVectorNear(FVec3 actual, FVec3 expected, f32 tolerance) noexcept
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

/** 結果の全公開fieldをobject representationに依存せず比較する。 */
void ExpectResultEqual(const FKinematicCharacterMovementResult3D& actual, const FKinematicCharacterMovementResult3D& expected) noexcept
{
    ExpectVectorNear(actual.NextState.Position, expected.NextState.Position, 0.0f);
    ExpectVectorNear(actual.NextState.Velocity, expected.NextState.Velocity, 0.0f);
    ExpectVectorNear(actual.NextState.GroundNormal, expected.NextState.GroundNormal, 0.0f);
    EXPECT_EQ(actual.NextState.Grounded, expected.NextState.Grounded);
    ExpectVectorNear(actual.Translation, expected.Translation, 0.0f);
    EXPECT_EQ(actual.LastCollisionShape.Packed, expected.LastCollisionShape.Packed);
    ExpectVectorNear(actual.LastCollisionNormal, expected.LastCollisionNormal, 0.0f);
    EXPECT_EQ(actual.CollisionCount, expected.CollisionCount);
    EXPECT_EQ(actual.DepenetrationIterationCount, expected.DepenetrationIterationCount);
    EXPECT_EQ(actual.Jumped, expected.Jumped);
    EXPECT_EQ(actual.Depenetrated, expected.Depenetrated);
    EXPECT_EQ(actual.HitGround, expected.HitGround);
    EXPECT_EQ(actual.HitWall, expected.HitWall);
    EXPECT_EQ(actual.HitCeiling, expected.HitCeiling);
    EXPECT_EQ(actual.SlideIterationLimitReached, expected.SlideIterationLimitReached);
}

/** 重力を使わない基本調整値を返す。 */
FKinematicCharacterMovementParams3D NoGravityParams() noexcept
{
    FKinematicCharacterMovementParams3D params;
    params.GravityAcceleration = 0.0f;
    return params;
}

/** 上面Y=0の広い床を登録する。 */
FCollisionShapeId3D AddFloor(CCollisionWorld3D& world, u32 layer = CCollisionWorld3D::kAllLayers) noexcept
{
    return world.TryAddAabb(FAabb3{FVec3{0.0f, -0.5f, 0.0f}, FVec3{10.0f, 0.5f, 10.0f}}, layer);
}

} // namespace

ACS_TEST(KinematicCharacterMovement3D, PublicContractUsesValueTypesAndNoVirtualService)
{
    static_assert(std::is_trivially_copyable_v<FKinematicCharacterMovementInput3D>);
    static_assert(std::is_trivially_copyable_v<FKinematicCharacterState3D>);
    static_assert(std::is_trivially_copyable_v<FKinematicCharacterMovementParams3D>);
    static_assert(std::is_trivially_copyable_v<FKinematicCharacterMovementResult3D>);
    static_assert(std::is_same_v<decltype(&TryMoveKinematicCharacter3D), bool (*)(const CCollisionWorld3D&, const FKinematicCharacterMovementInput3D&, const FKinematicCharacterState3D&, f32, const FKinematicCharacterMovementParams3D&, FKinematicCharacterMovementResult3D&) noexcept>);
    EXPECT_TRUE(true);
}

ACS_TEST(KinematicCharacterMovement3D, EmptyWorldAppliesHorizontalVelocityAndGravity)
{
    CCollisionWorld3D world;
    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity = FVec2{4.0f, -2.0f};
    FKinematicCharacterState3D state;
    state.Position = FVec3{1.0f, 2.0f, 3.0f};
    state.Velocity = FVec3{0.0f, 1.0f, 0.0f};
    FKinematicCharacterMovementParams3D params;
    params.GravityAcceleration = 2.0f;
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, state, 0.5f, params, result));
    ExpectVectorNear(result.NextState.Position, FVec3{3.0f, 2.0f, 2.0f}, 1.0e-6f);
    ExpectVectorNear(result.NextState.Velocity, FVec3{4.0f, 0.0f, -2.0f}, 1.0e-6f);
    ExpectVectorNear(result.Translation, FVec3{2.0f, 0.0f, -1.0f}, 1.0e-6f);
    EXPECT_FALSE(result.NextState.Grounded);
    EXPECT_EQ(result.CollisionCount, 0u);
}

ACS_TEST(KinematicCharacterMovement3D, FloorProbePublishesStableGroundState)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D floor = AddFloor(world);
    EXPECT_TRUE(floor.IsValid());
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.0f, 0.51f, 0.0f};
    const FKinematicCharacterMovementParams3D params = NoGravityParams();
    FKinematicCharacterMovementResult3D first;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, {}, state, 0.0f, params, first));
    EXPECT_TRUE(first.NextState.Grounded);
    EXPECT_TRUE(first.HitGround);
    EXPECT_TRUE(first.LastCollisionShape == floor);
    ExpectVectorNear(first.NextState.GroundNormal, FVec3::Up(), 1.0e-6f);
    EXPECT_NEAR(first.NextState.Position.y, 0.501f, 1.0e-6f);

    FKinematicCharacterMovementResult3D second;
    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, {}, first.NextState, 1.0f / 60.0f, params, second));
    EXPECT_TRUE(second.NextState.Grounded);
    EXPECT_NEAR(second.NextState.Position.y, first.NextState.Position.y, 1.0e-6f);
}

ACS_TEST(KinematicCharacterMovement3D, FloorProbeIgnoresSideWallContact)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D wall = world.TryAddAabb(FAabb3{FVec3{1.5f, 1.0f, 0.0f}, FVec3{0.5f, 2.0f, 2.0f}});
    const FCollisionShapeId3D floor = AddFloor(world);
    EXPECT_TRUE(wall.IsValid());
    EXPECT_TRUE(floor.IsValid());
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.5f, 0.501f, 0.0f};
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, {}, state, 0.0f, NoGravityParams(), result));
    EXPECT_TRUE(result.NextState.Grounded);
    EXPECT_TRUE(result.HitGround);
    EXPECT_TRUE(result.LastCollisionShape == floor);
    ExpectVectorNear(result.NextState.GroundNormal, FVec3::Up(), 1.0e-6f);
}

ACS_TEST(KinematicCharacterMovement3D, WallContactProjectsRemainingMovementAndVelocity)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D wall = world.TryAddAabb(FAabb3{FVec3{1.5f, 0.0f, 0.0f}, FVec3{0.5f, 10.0f, 10.0f}});
    EXPECT_TRUE(wall.IsValid());
    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity = FVec2{1.0f, 1.0f};
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, {}, 1.0f, NoGravityParams(), result));
    EXPECT_TRUE(result.HitWall);
    EXPECT_TRUE(result.LastCollisionShape == wall);
    EXPECT_NEAR(result.NextState.Position.x, 0.499f, 1.0e-6f);
    EXPECT_NEAR(result.NextState.Position.z, 1.0f, 1.0e-6f);
    EXPECT_NEAR(result.NextState.Velocity.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.NextState.Velocity.z, 1.0f, 1.0e-6f);
    EXPECT_FALSE(result.SlideIterationLimitReached);
}

ACS_TEST(KinematicCharacterMovement3D, InitialPenetrationIsResolvedBeforeGrounding)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(AddFloor(world).IsValid());
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.0f, 0.25f, 0.0f};
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, {}, state, 0.0f, NoGravityParams(), result));
    EXPECT_TRUE(result.Depenetrated);
    EXPECT_EQ(result.DepenetrationIterationCount, 1u);
    EXPECT_TRUE(result.NextState.Grounded);
    EXPECT_NEAR(result.NextState.Position.y, 0.501f, 1.0e-6f);
}

ACS_TEST(KinematicCharacterMovement3D, CollisionMaskSelectsRegisteredLayers)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D ignored = world.TryAddAabb(FAabb3{FVec3{1.5f, 0.0f, 0.0f}, FVec3{0.5f, 10.0f, 10.0f}}, 0x1u);
    const FCollisionShapeId3D selected = world.TryAddAabb(FAabb3{FVec3{3.0f, 0.0f, 0.0f}, FVec3{0.5f, 10.0f, 10.0f}}, 0x2u);
    EXPECT_TRUE(ignored.IsValid());
    EXPECT_TRUE(selected.IsValid());
    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity = FVec2{4.0f, 0.0f};
    input.CollisionMask = 0x2u;
    FKinematicCharacterMovementResult3D selected_result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, {}, 1.0f, NoGravityParams(), selected_result));
    EXPECT_TRUE(selected_result.LastCollisionShape == selected);
    EXPECT_NEAR(selected_result.NextState.Position.x, 1.999f, 1.0e-6f);

    input.CollisionMask = 0u;
    FKinematicCharacterMovementResult3D unfiltered_result;
    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, {}, 1.0f, NoGravityParams(), unfiltered_result));
    EXPECT_NEAR(unfiltered_result.NextState.Position.x, 4.0f, 1.0e-6f);
    EXPECT_EQ(unfiltered_result.CollisionCount, 0u);
}

ACS_TEST(KinematicCharacterMovement3D, SelfShapeIsExcludedFromAllQueries)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D self = world.TryAddSphere(FSphere{FVec3{}, 0.5f}, 0x4u);
    EXPECT_TRUE(self.IsValid());
    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity = FVec2{1.0f, 0.0f};
    input.SelfShape = self;
    input.CollisionMask = 0x4u;
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, {}, 1.0f, NoGravityParams(), result));
    EXPECT_NEAR(result.NextState.Position.x, 1.0f, 1.0e-6f);
    EXPECT_EQ(result.CollisionCount, 0u);
    EXPECT_TRUE(world.IsAlive(self));
}

ACS_TEST(KinematicCharacterMovement3D, GroundedJumpAndAirborneGravityUpdateVerticalVelocity)
{
    CCollisionWorld3D floor_world;
    EXPECT_TRUE(AddFloor(floor_world).IsValid());
    FKinematicCharacterState3D grounded_state;
    grounded_state.Position = FVec3{0.0f, 0.501f, 0.0f};
    grounded_state.GroundNormal = FVec3::Up();
    grounded_state.Grounded = true;
    FKinematicCharacterMovementInput3D jump_input;
    jump_input.JumpRequested = true;
    FKinematicCharacterMovementParams3D params;
    params.GravityAcceleration = 10.0f;
    params.JumpSpeed = 5.0f;
    FKinematicCharacterMovementResult3D jump_result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(floor_world, jump_input, grounded_state, 0.1f, params, jump_result));
    EXPECT_TRUE(jump_result.Jumped);
    EXPECT_FALSE(jump_result.NextState.Grounded);
    EXPECT_NEAR(jump_result.NextState.Velocity.y, 4.0f, 1.0e-6f);
    EXPECT_NEAR(jump_result.NextState.Position.y, 0.901f, 1.0e-6f);

    CCollisionWorld3D empty_world;
    FKinematicCharacterState3D airborne_state;
    airborne_state.Position = FVec3{0.0f, 2.0f, 0.0f};
    FKinematicCharacterMovementResult3D gravity_result;
    EXPECT_TRUE(TryMoveKinematicCharacter3D(empty_world, {}, airborne_state, 0.1f, params, gravity_result));
    EXPECT_NEAR(gravity_result.NextState.Velocity.y, -1.0f, 1.0e-6f);
    EXPECT_NEAR(gravity_result.NextState.Position.y, 1.9f, 1.0e-6f);
}

ACS_TEST(KinematicCharacterMovement3D, ZeroJumpSpeedKeepsGroundedWithoutPublishingJump)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(AddFloor(world).IsValid());
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.0f, 0.501f, 0.0f};
    state.GroundNormal = FVec3::Up();
    state.Grounded = true;
    FKinematicCharacterMovementInput3D input;
    input.JumpRequested = true;
    FKinematicCharacterMovementParams3D params = NoGravityParams();
    params.JumpSpeed = 0.0f;
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, state, 0.0f, params, result));
    EXPECT_FALSE(result.Jumped);
    EXPECT_TRUE(result.NextState.Grounded);
    EXPECT_TRUE(result.HitGround);
}

ACS_TEST(KinematicCharacterMovement3D, CeilingContactStopsUpwardVelocity)
{
    CCollisionWorld3D world;
    const FCollisionShapeId3D ceiling = world.TryAddAabb(FAabb3{FVec3{0.0f, 2.0f, 0.0f}, FVec3{2.0f, 0.5f, 2.0f}});
    EXPECT_TRUE(ceiling.IsValid());
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.0f, 0.5f, 0.0f};
    state.Velocity = FVec3{0.0f, 3.0f, 0.0f};
    FKinematicCharacterMovementResult3D result;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, {}, state, 0.5f, NoGravityParams(), result));
    EXPECT_TRUE(result.HitCeiling);
    EXPECT_TRUE(result.LastCollisionShape == ceiling);
    EXPECT_NEAR(result.NextState.Position.y, 0.999f, 1.0e-6f);
    EXPECT_NEAR(result.NextState.Velocity.y, 0.0f, 1.0e-6f);
}

ACS_TEST(KinematicCharacterMovement3D, InvalidAndNonFiniteInputsPreserveOutput)
{
    CCollisionWorld3D world;
    FKinematicCharacterMovementResult3D result;
    result.NextState.Position = FVec3{1.0f, 2.0f, 3.0f};
    result.NextState.Velocity = FVec3{4.0f, 5.0f, 6.0f};
    result.Translation = FVec3{7.0f, 8.0f, 9.0f};
    result.CollisionCount = 10u;
    result.HitWall = true;
    u8 preserved[sizeof(result)];
    MemCopy(preserved, &result, sizeof(result));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity.x = nan;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, input, {}, 1.0f, {}, result));
    FKinematicCharacterState3D state;
    state.Position.x = infinity;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, state, 1.0f, {}, result));
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, nan, {}, result));
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, -0.1f, {}, result));
    FKinematicCharacterMovementParams3D params;
    params.Radius = 0.0f;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, 1.0f, params, result));
    params = {};
    params.ContactOffset = 0.0f;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, 1.0f, params, result));
    params = {};
    params.ContactOffset = params.Radius;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, 1.0f, params, result));
    params = {};
    params.Radius = std::numeric_limits<f32>::max();
    params.ContactOffset = std::numeric_limits<f32>::max() * 0.75f;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, 1.0f, params, result));
    params = {};
    params.MinimumGroundNormalY = 1.1f;
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, {}, {}, 1.0f, params, result));
    input = {};
    input.DesiredHorizontalVelocity.x = std::numeric_limits<f32>::max();
    EXPECT_FALSE(TryMoveKinematicCharacter3D(world, input, {}, std::numeric_limits<f32>::max(), {}, result));
    EXPECT_TRUE(MemCmp(preserved, &result, sizeof(result)) == 0);
}

ACS_TEST(KinematicCharacterMovement3D, IdenticalInputAndWorldProduceEqualPublicValues)
{
    CCollisionWorld3D world;
    EXPECT_TRUE(AddFloor(world, 0x1u).IsValid());
    EXPECT_TRUE(world.TryAddAabb(FAabb3{FVec3{2.0f, 1.0f, 0.0f}, FVec3{0.5f, 2.0f, 2.0f}}, 0x1u).IsValid());
    FKinematicCharacterMovementInput3D input;
    input.DesiredHorizontalVelocity = FVec2{2.0f, 0.5f};
    input.CollisionMask = 0x1u;
    FKinematicCharacterState3D state;
    state.Position = FVec3{0.0f, 0.501f, 0.0f};
    state.GroundNormal = FVec3::Up();
    state.Grounded = true;
    FKinematicCharacterMovementResult3D first;
    FKinematicCharacterMovementResult3D second;

    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, state, 0.5f, {}, first));
    EXPECT_TRUE(TryMoveKinematicCharacter3D(world, input, state, 0.5f, {}, second));
    ExpectResultEqual(first, second);
}
