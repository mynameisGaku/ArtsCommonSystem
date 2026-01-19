#pragma once

#include <Pch.h>
#include "GameObject.h"

class Transform : public GO_Component
{
public:
    const ACSU_Math::Matrix4x4& GetWorldMatrix()
    {
        UpdateIfNeeded();
        return m_WorldMatrix;
    }

    const ACSU_Math::Matrix4x4& GetLocalMatrix()
    {
        UpdateIfNeeded();
        return m_LocalMatrix;
    }

    const ACSU_Math::Vector3 GetWorldPosition()
    {
        UpdateIfNeeded();
        return ACSU_Math::Vector3(
            m_WorldMatrix.m30,
            m_WorldMatrix.m31,
            m_WorldMatrix.m32
        );
    }

    const ACSU_Math::Quaternion GetWorldRotation()
    {
        UpdateIfNeeded();
        ACSU_Math::Vector3 pos, scale;
        ACSU_Math::Quaternion rot;
        ACSU_Math::DecomposeTransform(m_WorldMatrix, pos, rot, scale);
        return rot;
    }

    const ACSU_Math::Vector3 GetWorldScale()
    {
        UpdateIfNeeded();
        ACSU_Math::Vector3 pos, scale;
        ACSU_Math::Quaternion rot;
        ACSU_Math::DecomposeTransform(m_WorldMatrix, pos, rot, scale);
        return scale;
    }

    const ACSU_Math::Vector3 Right()
    {
        UpdateIfNeeded();
        return ACSU_Math::Vector3(
            m_WorldMatrix.m00,
            m_WorldMatrix.m10,
            m_WorldMatrix.m20
        ).normalized();
	}

    const ACSU_Math::Vector3 Up()
    {
        UpdateIfNeeded();
        return ACSU_Math::Vector3(
            m_WorldMatrix.m01,
            m_WorldMatrix.m11,
            m_WorldMatrix.m21
        ).normalized();
    }

    const ACSU_Math::Vector3 Forward()
    {
        UpdateIfNeeded();
        return ACSU_Math::Vector3(
            m_WorldMatrix.m02,
            m_WorldMatrix.m12,
            m_WorldMatrix.m22
        ).normalized();
	}

    const std::uint32_t GetWorldVersion() const
    {
        return m_WorldVersion;
	}

    void SetLocalPosition(const ACSU_Math::Vector3& v)
    {
        m_LocalPosition = v;
        MarkLocalDirty();
    }

    void SetLocalRotation(const ACSU_Math::Quaternion& q)
    {
        m_LocalRotation = q;
        MarkLocalDirty();
    }

    void Move(const Vector3& move)
    {
        m_LocalPosition = (m_LocalPosition + move);
        MarkLocalDirty();
    }

    void SetLocalScale(const ACSU_Math::Vector3& v)
    {
        m_LocalScale = v;
        MarkLocalDirty();
    }

    void RotateLocal(const ACSU_Math::Quaternion& delta)
    {
        m_LocalRotation = (m_LocalRotation * delta).normalized();
        MarkLocalDirty();
    }

    void RotateEulerLocal(const ACSU_Math::Vector3& eulerDegrees)
    {
        ACSU_Math::Quaternion delta = ACSU_Math::Quaternion::Euler(
            eulerDegrees.x,
            eulerDegrees.y,
            eulerDegrees.z
        );

        m_LocalRotation = (m_LocalRotation * delta).normalized();
        MarkLocalDirty();
    }
    
    void LookAt(const ACSU_Math::Vector3& target, const ACSU_Math::Vector3& worldUp = ACSU_Math::Vector3::up());
    void SetWorldRotation(const ACSU_Math::Quaternion& worldRotation);
    
    const ACSU_Math::Vector3& GetLocalPosition() const { return m_LocalPosition; }
    const ACSU_Math::Quaternion& GetLocalRotation() const { return m_LocalRotation; }
    const ACSU_Math::Vector3& GetLocalScale() const { return m_LocalScale; }

    void SetParent(Transform* parent, bool keepWorld)
    {
        if (parent == m_Parent)
        {
            return;
        }

        const ACSU_Math::Matrix4x4 oldWorld = GetWorldMatrix();

        DetachFromParent();
        m_Parent = parent;
        AttachToParent();

        if (keepWorld)
        {
            ACSU_Math::Matrix4x4 parentWorld;

            if (m_Parent != nullptr)
            {
                parentWorld = m_Parent->GetWorldMatrix();
            }
            else
            {
                parentWorld = ACSU_Math::Matrix4x4::identity();
            }

            const ACSU_Math::Matrix4x4 newLocal =
                ACSU_Math::InverseMatrix(parentWorld) * oldWorld;

            ACSU_Math::DecomposeTransform(
                newLocal,
                m_LocalPosition,
                m_LocalRotation,
                m_LocalScale
            );

            m_LocalMatrix = newLocal;
            m_LocalDirty = false;
        }

        MarkWorldDirtyRecursive();
    }

    const std::vector<Transform*>& GetChildren() const
    {
        return m_Children;
    }

public:
    void Update(float) override
    {
        UpdateIfNeeded();
    }

private:
    void UpdateIfNeeded()
    {
        if (m_LocalDirty)
        {
            m_LocalMatrix = ACSU_Math::MakeTransform(m_LocalPosition, m_LocalRotation, m_LocalScale);
            m_LocalDirty = false;
            m_WorldDirty = true;
        }

        if (m_WorldDirty)
        {
            if (m_Parent != nullptr)
            {
                m_WorldMatrix = m_LocalMatrix * m_Parent->GetWorldMatrix();
            }
            else
            {
                m_WorldMatrix = m_LocalMatrix;
            }

            m_WorldDirty = false;
            m_WorldVersion++;
        }
    }

    void MarkLocalDirty()
    {
        m_LocalDirty = true;
        MarkWorldDirtyRecursive();
    }

    void MarkWorldDirtyRecursive()
    {
        m_WorldDirty = true;

        for (Transform* c : m_Children)
        {
            if (c != nullptr)
            {
                c->MarkWorldDirtyRecursive();
            }
        }
    }

    void AttachToParent()
    {
        if (m_Parent == nullptr)
        {
            return;
        }

        m_Parent->m_Children.push_back(this);
    }

    void DetachFromParent()
    {
        if (m_Parent == nullptr)
        {
            return;
        }

        auto& v = m_Parent->m_Children;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }

private:
    ACSU_Math::Vector3 m_LocalPosition = ACSU_Math::Vector3::zero();
    ACSU_Math::Quaternion m_LocalRotation = ACSU_Math::Quaternion::identity();
    ACSU_Math::Vector3 m_LocalScale = ACSU_Math::Vector3::one();

    ACSU_Math::Matrix4x4 m_LocalMatrix;
    ACSU_Math::Matrix4x4 m_WorldMatrix;

    bool m_LocalDirty = true;
    bool m_WorldDirty = true;

    std::uint32_t m_WorldVersion = 0;
    std::uint32_t m_LastPushedWorldVersion = 0;

    Transform* m_Parent = nullptr;
    std::vector<Transform*> m_Children;
};
