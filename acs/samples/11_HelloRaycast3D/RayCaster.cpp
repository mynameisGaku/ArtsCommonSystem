// SPDX-License-Identifier: Apache-2.0
#include "RayCaster.h"

#include "RaycastTargets.h"

#include "platform/Input.h"
#include "math/Math.h"
#include "math/Collision3D.h"

using namespace acs;

namespace helloraycast3d {

void RayCaster::Init(f32 aspect) noexcept {
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    _cam_pos = kCamInitialPos;
}

void RayCaster::Update(f32 dt, RaycastTargets& targets) noexcept {
    // ---- カメラ回転 / 移動 ----
    const f32 move_speed = kCamMoveSpeed * dt;
    const f32 turn_speed = kCamTurnSpeed * dt;
    if (FInput::IsKeyDown(EKey::Left))  _cam_yaw   -= turn_speed;
    if (FInput::IsKeyDown(EKey::Right)) _cam_yaw   += turn_speed;
    if (FInput::IsKeyDown(EKey::Up))    _cam_pitch -= turn_speed * 0.8f;
    if (FInput::IsKeyDown(EKey::Down))  _cam_pitch += turn_speed * 0.8f;

    // pitch を ±81° 程度に clamp してジンバルロックを回避
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;

    const FVec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                       -Sin(_cam_pitch),
                        Cos(_cam_yaw) * Cos(_cam_pitch) };
    const FVec3 right  { Cos(_cam_yaw), 0, -Sin(_cam_yaw) };

    if (FInput::IsKeyDown(EKey::W)) _cam_pos += forward * move_speed;
    if (FInput::IsKeyDown(EKey::S)) _cam_pos -= forward * move_speed;
    if (FInput::IsKeyDown(EKey::D)) _cam_pos += right   * move_speed;
    if (FInput::IsKeyDown(EKey::A)) _cam_pos -= right   * move_speed;

    _cam_forward = forward;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);

    // ---- レイ vs オブジェクト (最近傍ヒット) ----
    // best_t を 1000 で初期化 → ヒット候補の中で最も手前を残す。
    FRay3 ray{ _cam_pos, forward };
    i32  best_index = -1;
    FVec3 best_point{};
    f32  best_t = 1000.0f;
    const u32 n = targets.Count();
    for (u32 i = 0; i < n; ++i) {
        const Object& o = targets.At(i);
        FRayHit3 h{};
        if (o.kind == ShapeKind::FSphere) {
            const FSphere s{ o.position, o.radius_or_half };
            h = RaycastSphere(ray, s, best_t);
        } else {
            const FAabb3 box = FAabb3::FromCenterExtents(
                o.position,
                FVec3{o.radius_or_half, o.radius_or_half, o.radius_or_half});
            h = RaycastAabb(ray, box, best_t);
        }
        if (h.hit && h.t < best_t) {
            best_t     = h.t;
            best_index = static_cast<i32>(i);
            best_point = h.point;
        }
    }

    if (best_index >= 0) targets.SetHit(best_index, best_point);
    else                 targets.ClearHit();
}

} // namespace helloraycast3d
