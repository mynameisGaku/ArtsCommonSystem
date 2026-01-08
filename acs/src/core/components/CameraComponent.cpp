#include "CameraComponent.h"

#if __has_include(<DxLib.h>)
#define ACSU_HAS_DXLIB 1
#else
#define ACSU_HAS_DXLIB 0
#endif

CameraComponent::CameraComponent()
{
}

void CameraComponent::Start()
{
#if ACSU_HAS_DXLIB
    // DxLibの初期値を入れておく（必要ならScene初期化側に寄せても良い）
    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}

void CameraComponent::Draw(float)
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
    // forward = (0,0,1) を回転で向ける
    const ACSU_Math::Vector3 forwardLocal = trs.forward;
    const ACSU_Math::Vector3 forward = trs.rotation.Euler(forwardLocal).eulerAngles();

    VECTOR pos;
    pos.x = trs.position.x;
    pos.y = trs.position.y;
    pos.z = trs.position.z;

    VECTOR target;
    target.x = trs.position.x + forward.x;
    target.y = trs.position.y + forward.y;
    target.z = trs.position.z + forward.z;

    // DxLibの「UpVecY」版を使う（up=(0,1,0)固定）
    SetCameraPositionAndTargetAndUpVec(pos, target, VECTOR(trs.up.x, trs.up.y, trs.up.z));

    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}
