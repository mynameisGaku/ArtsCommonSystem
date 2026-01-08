#pragma once

#include <Pch.h>
#include "GameObject.h"

class Transform : public Component
{
public:
    const ACSU_Math::Mat4& GetWorldMatrix()
    {
        UpdateIfNeeded();
        return m_WorldMatrix;
    }

    void SetLocalPosition(const ACSU_Math::Vec3& v)
    {
        m_LocalPosition = v;
        MarkLocalDirty();
    }

    void SetLocalRotation(const ACSU_Math::Quat& q)
    {
        m_LocalRotation = q;
        MarkLocalDirty();
    }

    void SetLocalScale(const ACSU_Math::Vec3& v)
    {
        m_LocalScale = v;
        MarkLocalDirty();
    }

    const ACSU_Math::Vec3& GetLocalPosition() const { return m_LocalPosition; }
    const ACSU_Math::Quat& GetLocalRotation() const { return m_LocalRotation; }
    const ACSU_Math::Vec3& GetLocalScale() const { return m_LocalScale; }

    void SetParent(Transform* parent, bool keepWorld)
    {
        if (parent == m_Parent)
        {
            return;
        }

        const ACSU_Math::Mat4 oldWorld = GetWorldMatrix();

        DetachFromParent();
        m_Parent = parent;
        AttachToParent();

        if (keepWorld)
        {
            ACSU_Math::Mat4 parentWorld;

            if (m_Parent != nullptr)
            {
                parentWorld = m_Parent->GetWorldMatrix();
            }
            else
            {
                parentWorld = ACSU_Math::Mat4::Identity();
            }

            const ACSU_Math::Mat4 newLocal =
                ACSU_Math::InverseMatrix(parentWorld) * oldWorld;

            ACSU_Math::DecomposeTRS(
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
        WriteWorldTRSToBlackboardIfChanged();
    }

private:
    void UpdateIfNeeded()
    {
        if (m_LocalDirty)
        {
            m_LocalMatrix = ACSU_Math::MakeTRS(
                m_LocalPosition,
                m_LocalRotation,
                m_LocalScale
            );
            m_LocalDirty = false;
            m_WorldDirty = true;
        }

        if (m_WorldDirty)
        {
            if (m_Parent != nullptr)
            {
                m_WorldMatrix = m_Parent->GetWorldMatrix() * m_LocalMatrix;
            }
            else
            {
                m_WorldMatrix = m_LocalMatrix;
            }

            m_WorldDirty = false;
            m_WorldVersion++;
        }
    }

    void WriteWorldTRSToBlackboardIfChanged();

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
    ACSU_Math::Vec3 m_LocalPosition = ACSU_Math::Vec3::Zero();
    ACSU_Math::Quat m_LocalRotation = ACSU_Math::Quat::Identity();
    ACSU_Math::Vec3 m_LocalScale = ACSU_Math::Vec3::One();

    ACSU_Math::Mat4 m_LocalMatrix;
    ACSU_Math::Mat4 m_WorldMatrix;

    bool m_LocalDirty = true;
    bool m_WorldDirty = true;

    std::uint32_t m_WorldVersion = 0;
    std::uint32_t m_LastPushedWorldVersion = 0;

    Transform* m_Parent = nullptr;
    std::vector<Transform*> m_Children;
};
