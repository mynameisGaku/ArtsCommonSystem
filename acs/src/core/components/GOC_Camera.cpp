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

    TransformTRS trs;
    if (!go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, trs))
    {
        return;
    }

#if ACSU_HAS_DXLIB
    const ACSU_Math::Vector3 forward = trs.forward;

    VECTOR pos;
    pos.x = trs.position.x;
    pos.y = trs.position.y;
    pos.z = trs.position.z;

    VECTOR target;
    target.x = trs.position.x + forward.x;
    target.y = trs.position.y + forward.y;
    target.z = trs.position.z + forward.z;

    SetCameraPositionAndTargetAndUpVec(pos, target, VECTOR(trs.up.x, trs.up.y, trs.up.z));

    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}
