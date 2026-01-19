#include "GOC_Camera.h"

#if __has_include(<DxLib.h>)
#define ACSU_HAS_DXLIB 1
#else
#define ACSU_HAS_DXLIB 0
#endif

GOC_Camera::GOC_Camera()
{
}

void GOC_Camera::Start()
{
#if ACSU_HAS_DXLIB
    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}

void GOC_Camera::Draw(float)
{
    if (!m_IsMain)
    {
        return;
    }

    auto* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    Transform* trs = go->GetGOC<Transform>();
    if (!trs) return;

    go->GetSceneContext().MainCameraPosition = trs->GetWorldPosition();

#if ACSU_HAS_DXLIB
    const ACSU_Math::Vector3 forward = trs->Forward();

    VECTOR pos;
    pos.x = trs->GetWorldPosition().x;
    pos.y = trs->GetWorldPosition().y;
    pos.z = trs->GetWorldPosition().z;

    VECTOR target;
    target.x = trs->GetWorldPosition().x + forward.x;
    target.y = trs->GetWorldPosition().y + forward.y;
    target.z = trs->GetWorldPosition().z + forward.z;

    SetCameraPositionAndTargetAndUpVec(pos, target, VECTOR(trs->Up().x, trs->Up().y, trs->Up().z));

    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}
