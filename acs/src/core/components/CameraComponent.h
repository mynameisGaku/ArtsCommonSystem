#pragma once

#include <Pch.h>

class CameraComponent : public Component
{
public:
    CameraComponent();

    void Start() override;
    void Draw(float deltaTime) override;

    bool IsMain() const
    {
        return m_IsMain;
    }

    void SetMain(bool isMain)
    {
        m_IsMain = isMain;
    }

    void SetNearFar(float nearZ, float farZ)
    {
        m_NearZ = nearZ;
        m_FarZ = farZ;
    }

    void SetFov(float fov)
    {
        m_FovY = fov * 3.1415926535f / 180.0f;
    }

private:
    bool m_IsMain = true;

    float m_FovY = 60.0f * 3.1415926535f / 180.0f;
    float m_NearZ = 0.1f;
    float m_FarZ = 1000.0f;
};
