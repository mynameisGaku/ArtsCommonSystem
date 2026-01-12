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

// CameraComponent.cpp

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
    // 【修正】trs.forward は既に行列から計算された安定した前方ベクトルなので、そのまま使う
    // 余計な変換（EulerやeulerAngles）を通すとジンバルロックや計算誤差の原因になります
    const ACSU_Math::Vector3 forward = trs.forward;

    VECTOR pos;
    pos.x = trs.position.x;
    pos.y = trs.position.y;
    pos.z = trs.position.z;

    VECTOR target;
    target.x = trs.position.x + forward.x;
    target.y = trs.position.y + forward.y;
    target.z = trs.position.z + forward.z;

    // Upベクトルも Transform から取得したものを使用（回転に対応するため）
    SetCameraPositionAndTargetAndUpVec(pos, target, VECTOR(trs.up.x, trs.up.y, trs.up.z));

    SetupCamera_Perspective(m_FovY);
    SetCameraNearFar(m_NearZ, m_FarZ);
#endif
}
