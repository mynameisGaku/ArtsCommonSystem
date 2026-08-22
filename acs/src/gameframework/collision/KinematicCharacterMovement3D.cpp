// SPDX-License-Identifier: Apache-2.0
// 既存3D collision queryを組み合わせ、sphere型characterの決定的な移動を計算する。
#include "gameframework/collision/KinematicCharacterMovement3D.h"

#include "gameframework/SpherePenetrationResolution3D.h"

#include <cmath>

namespace acs::game {
namespace {

/** 一回の移動前後で許可する貫通分離回数。 */
constexpr u32 kDepenetrationIterations = 4u;

/** 一回の移動で許可するsweepと接触面投影の回数。 */
constexpr u32 kSlideIterations = 4u;

/** 3成分がすべて有限ならtrueを返す。 */
bool IsFiniteVector3_Internal(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** 2成分がすべて有限ならtrueを返す。 */
bool IsFiniteVector2_Internal(FVec2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

/** 入力状態が有限ならtrueを返す。 */
bool IsValidState_Internal(const FKinematicCharacterState3D& state) noexcept
{
    return IsFiniteVector3_Internal(state.Position) && IsFiniteVector3_Internal(state.Velocity) && IsFiniteVector3_Internal(state.GroundNormal);
}

/** 移動調整値と導出距離が有限で安全ならtrueを返す。 */
bool IsValidParams_Internal(const FKinematicCharacterMovementParams3D& params) noexcept
{
    if (!std::isfinite(params.Radius) || params.Radius <= 0.0f) return false;
    if (!std::isfinite(params.GravityAcceleration) || params.GravityAcceleration < 0.0f) return false;
    if (!std::isfinite(params.JumpSpeed) || params.JumpSpeed < 0.0f) return false;
    if (!std::isfinite(params.ContactOffset) || params.ContactOffset <= 0.0f || params.ContactOffset >= params.Radius) return false;
    if (!std::isfinite(params.GroundProbeDistance) || params.GroundProbeDistance < 0.0f) return false;
    if (!std::isfinite(params.MinimumGroundNormalY) || params.MinimumGroundNormalY < 0.0f || params.MinimumGroundNormalY > 1.0f) return false;
    const f32 probe_restore_offset = params.ContactOffset + params.ContactOffset;
    const f32 probe_radius = params.Radius - params.ContactOffset;
    const f32 probe_distance = params.GroundProbeDistance + probe_restore_offset;
    return std::isfinite(probe_restore_offset) && std::isfinite(probe_radius) && probe_radius > 0.0f && std::isfinite(probe_distance);
}

/** 移動量の法線へ食い込む成分だけを除き、接触面へ投影する。 */
void ProjectAwayFromSurface_Internal(FVec3 normal, FVec3& value) noexcept
{
    const f32 normal_component = Dot(value, normal);
    if (normal_component < 0.0f) value -= normal * normal_component;
}

/** 接触法線を歩行面、壁、天井へ分類し、結果と接地状態へ記録する。 */
void RecordContact_Internal(FCollisionShapeId3D shape, FVec3 normal, const FKinematicCharacterMovementParams3D& params, FKinematicCharacterMovementResult3D& result, bool& grounded, FVec3& ground_normal) noexcept
{
    result.LastCollisionShape = shape;
    result.LastCollisionNormal = normal;
    ++result.CollisionCount;
    if (normal.y >= params.MinimumGroundNormalY) {
        result.HitGround = true;
        if (!grounded || normal.y > ground_normal.y) ground_normal = normal;
        grounded = true;
    } else if (normal.y <= -params.MinimumGroundNormalY) {
        result.HitCeiling = true;
    } else {
        result.HitWall = true;
    }
}

/** 固定回数でsphere貫通を解消し、未収束または非有限結果を失敗として返す。 */
bool TryDepenetrate_Internal(const CCollisionWorld3D& world, f32 radius, FCollisionShapeId3D self_shape, u32 mask, FVec3& position, FKinematicCharacterMovementResult3D& result) noexcept
{
    FSpherePenetrationResolution3D resolution;
    if (!TryResolveSpherePenetrations3D(world, FSphere{position, radius}, resolution, kDepenetrationIterations, self_shape, mask)) return false;
    if (!resolution.FullyResolved || !IsFiniteVector3_Internal(resolution.ResolvedSphere.center)) return false;
    position = resolution.ResolvedSphere.center;
    result.DepenetrationIterationCount += resolution.IterationCount;
    result.Depenetrated = result.DepenetrationIterationCount != 0u;
    return true;
}

/** 連続sphere sweepで最初の接触まで進み、残り移動と速度を接触面へ投影する。 */
bool TryMoveAndSlide_Internal(const CCollisionWorld3D& world, const FKinematicCharacterMovementInput3D& input, const FKinematicCharacterMovementParams3D& params, FVec3& position, FVec3& velocity, FVec3 displacement, FKinematicCharacterMovementResult3D& result, bool& grounded, FVec3& ground_normal) noexcept
{
    FVec3 remaining = displacement;
    for (u32 iteration = 0u; iteration < kSlideIterations; ++iteration) {
        const f32 remaining_length_squared = Dot(remaining, remaining);
        if (!std::isfinite(remaining_length_squared)) return false;
        if (remaining_length_squared == 0.0f) return true;

        FCollisionSweepHit3D hit;
        if (!world.TrySweepSphere(FRay3{position, remaining}, params.Radius, 0.0f, 1.0f, hit, input.SelfShape, input.CollisionMask)) {
            position += remaining;
            return IsFiniteVector3_Internal(position);
        }

        if (!hit.IsValid() || !std::isfinite(hit.T) || hit.T < 0.0f || hit.T > 1.0f || !IsFiniteVector3_Internal(hit.Center) || !IsFiniteVector3_Internal(hit.Normal)) return false;
        position = hit.Center + hit.Normal * params.ContactOffset;
        if (!IsFiniteVector3_Internal(position)) return false;
        RecordContact_Internal(hit.Shape, hit.Normal, params, result, grounded, ground_normal);

        const f32 remaining_fraction = 1.0f - hit.T;
        remaining *= remaining_fraction;
        ProjectAwayFromSurface_Internal(hit.Normal, remaining);
        ProjectAwayFromSurface_Internal(hit.Normal, velocity);
        if (!IsFiniteVector3_Internal(remaining) || !IsFiniteVector3_Internal(velocity)) return false;
    }

    const f32 remaining_length_squared = Dot(remaining, remaining);
    if (!std::isfinite(remaining_length_squared)) return false;
    result.SlideIterationLimitReached = remaining_length_squared > 0.0f;
    return true;
}

/** 非上昇時に一回だけ直下を調べ、歩行可能面なら接地位置と速度へ反映する。 */
bool TryDetectGround_Internal(const CCollisionWorld3D& world, const FKinematicCharacterMovementInput3D& input, const FKinematicCharacterMovementParams3D& params, FVec3& position, FVec3& velocity, FKinematicCharacterMovementResult3D& result, bool& grounded, FVec3& ground_normal) noexcept
{
    if (grounded || velocity.y > 0.0f) return true;
    // probeだけを接触間隔ぶん縮め、横壁のT=0接触より下方の床・斜面を優先できるようにする。
    const f32 probe_radius = params.Radius - params.ContactOffset;
    const f32 probe_restore_offset = params.ContactOffset + params.ContactOffset;
    const f32 probe_distance = params.GroundProbeDistance + probe_restore_offset;
    const FVec3 probe_displacement{0.0f, -probe_distance, 0.0f};
    FCollisionSweepHit3D hit;
    if (!world.TrySweepSphere(FRay3{position, probe_displacement}, probe_radius, 0.0f, 1.0f, hit, input.SelfShape, input.CollisionMask)) return true;
    if (!hit.IsValid() || !IsFiniteVector3_Internal(hit.Center) || !IsFiniteVector3_Internal(hit.Normal)) return false;
    if (hit.Normal.y < params.MinimumGroundNormalY) return true;

    // 縮小半径を元へ戻す距離と最終接触間隔を合わせ、通常sweepと同じ最終中心にする。
    position = hit.Center + hit.Normal * probe_restore_offset;
    if (!IsFiniteVector3_Internal(position)) return false;
    RecordContact_Internal(hit.Shape, hit.Normal, params, result, grounded, ground_normal);
    ProjectAwayFromSurface_Internal(hit.Normal, velocity);
    return IsFiniteVector3_Internal(velocity);
}

} // namespace

/** 検証済み入力から重力、jump、sweep、slide、接地を順に適用して次状態を確定する。 */
bool TryMoveKinematicCharacter3D(const CCollisionWorld3D& world, const FKinematicCharacterMovementInput3D& input, const FKinematicCharacterState3D& state, f32 delta_time, const FKinematicCharacterMovementParams3D& params, FKinematicCharacterMovementResult3D& out_result) noexcept
{
    if (!IsFiniteVector2_Internal(input.DesiredHorizontalVelocity) || !IsValidState_Internal(state)) return false;
    if (!std::isfinite(delta_time) || delta_time < 0.0f || !IsValidParams_Internal(params)) return false;

    FKinematicCharacterMovementResult3D result;
    FVec3 position = state.Position;
    if (!TryDepenetrate_Internal(world, params.Radius, input.SelfShape, input.CollisionMask, position, result)) return false;

    FVec3 velocity{input.DesiredHorizontalVelocity.x, state.Velocity.y, input.DesiredHorizontalVelocity.y};
    const bool jumped = input.JumpRequested && state.Grounded && params.JumpSpeed > 0.0f;
    if (jumped) {
        velocity.y = params.JumpSpeed;
        result.Jumped = true;
    } else if (state.Grounded && velocity.y < 0.0f) {
        velocity.y = 0.0f;
    }
    velocity.y -= params.GravityAcceleration * delta_time;
    const FVec3 displacement = velocity * delta_time;
    if (!IsFiniteVector3_Internal(velocity) || !IsFiniteVector3_Internal(displacement)) return false;

    bool grounded = false;
    FVec3 ground_normal{};
    if (!TryMoveAndSlide_Internal(world, input, params, position, velocity, displacement, result, grounded, ground_normal)) return false;
    if (!jumped && !TryDetectGround_Internal(world, input, params, position, velocity, result, grounded, ground_normal)) return false;
    if (!TryDepenetrate_Internal(world, params.Radius, input.SelfShape, input.CollisionMask, position, result)) return false;

    result.NextState.Position = position;
    result.NextState.Velocity = velocity;
    result.NextState.GroundNormal = grounded ? ground_normal : FVec3{};
    result.NextState.Grounded = grounded;
    result.Translation = position - state.Position;
    if (!IsValidState_Internal(result.NextState) || !IsFiniteVector3_Internal(result.Translation)) return false;

    out_result = result;
    return true;
}

} // namespace acs::game
