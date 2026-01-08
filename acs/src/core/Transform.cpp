#include "Pch.h"

void Transform::WriteWorldTRSToBlackboardIfChanged()
{
    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    if (m_LastPushedWorldVersion == m_WorldVersion)
    {
        return;
    }

    TransformTRS trs;
    trs.position = ACSU_Math::Vector3::zero();
    trs.rotation = ACSU_Math::Quaternion::identity();
    trs.scale = ACSU_Math::Vector3::one();

    m_WorldMatrix.DecomposeTRS(trs.position, trs.rotation, trs.scale);

    go->GetBlackboard().Set(BlackboardKeys::TransformWorldTRS, trs);

    m_LastPushedWorldVersion = m_WorldVersion;
}
