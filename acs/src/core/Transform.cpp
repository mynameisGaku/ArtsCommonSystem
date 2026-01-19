#include "Pch.h"

void Transform::SetWorldRotation(const ACSU_Math::Quaternion& worldRotation)
{
    if (m_Parent)
    {
        ACSU_Math::Quaternion parentRot = m_Parent->GetWorldRotation();
        m_LocalRotation = ACSU_Math::Quaternion::Inverse(parentRot) * worldRotation;
    }
    else
    {
        m_LocalRotation = worldRotation;
    }
    MarkLocalDirty();
}

void Transform::LookAt(const ACSU_Math::Vector3& target, const ACSU_Math::Vector3& worldUp)
{
    ACSU_Math::Vector3 worldPos = GetWorldPosition();
    ACSU_Math::Vector3 forward = target - worldPos;

    if (forward.sqrMagnitude() < ACSU_Math::Mathf::Epsilon)
    {
        return;
    }

    ACSU_Math::Quaternion rot = ACSU_Math::Quaternion::LookRotation(forward, worldUp);

    SetWorldRotation(rot);
}