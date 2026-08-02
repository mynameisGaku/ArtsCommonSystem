// SPDX-License-Identifier: Apache-2.0
#include "RayCaster.h"

#include "RaycastTargets.h"

#include "platform/Input.h"
#include "math/Math.h"
#include "math/Collision3D.h"

using namespace acs;

namespace helloraycast3d {

void CRayCaster::Init(f32 aspect) noexcept {
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    m_CamPos = kCamInitialPos;
}

void CRayCaster::Update(f32 dt, CRaycastTargets& targets) noexcept {
    // ---- カメラ回転 / 移動 ----
    const f32 move_speed = kCamMoveSpeed * dt;
    const f32 turn_speed = kCamTurnSpeed * dt;
    if (CInput::IsKeyDown(EKey::Left))  m_CamYaw   -= turn_speed;
    if (CInput::IsKeyDown(EKey::Right)) m_CamYaw   += turn_speed;
    if (CInput::IsKeyDown(EKey::Up))    m_CamPitch -= turn_speed * 0.8f;
    if (CInput::IsKeyDown(EKey::Down))  m_CamPitch += turn_speed * 0.8f;

    // pitch を ±81° 程度に clamp してジンバルロックを回避
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;

    const FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                       -Sin(m_CamPitch),
                        Cos(m_CamYaw) * Cos(m_CamPitch) };
    const FVec3 right  { Cos(m_CamYaw), 0, -Sin(m_CamYaw) };

    if (CInput::IsKeyDown(EKey::W)) m_CamPos += forward * move_speed;
    if (CInput::IsKeyDown(EKey::S)) m_CamPos -= forward * move_speed;
    if (CInput::IsKeyDown(EKey::D)) m_CamPos += right   * move_speed;
    if (CInput::IsKeyDown(EKey::A)) m_CamPos -= right   * move_speed;

    m_CamForward = forward;
    m_Camera.SetLookAt(m_CamPos, m_CamPos + forward);

    // ---- レイ vs オブジェクト (最近傍ヒット) ----
    // best_t を 1000 で初期化 → ヒット候補の中で最も手前を残す。
    FRay3 ray{ m_CamPos, forward };
    i32  best_index = -1;
    FVec3 best_point{};
    f32  best_t = 1000.0f;
    const u32 n = targets.Count();
    for (u32 i = 0; i < n; ++i) {
        const FRaycastTarget& o = targets.At(i);
        FRayHit3 h{};
        if (o.kind == EShapeKind::Sphere) {
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
