#pragma once
#include <Pch.h>

struct SceneContext
{
    ACSU_Math::Vector3 MainCameraPosition = { 0, 0, 0 };

    ACSU_Math::Vector3 SunDirection = { 0, -1, 0 };
    ACSU_Math::Vector3 SkyTint = { 1.0f, 1.0f, 1.0f };
    ACSU_Math::Vector3 GroundColor = { 0.37f, 0.35f, 0.34f };
    ACSU_Math::Vector3 HorizonColor = { 0.60f, 0.80f, 0.95f };

    float SunSize = 0.04f;
    float SunConvergence = 150.0f;
    float AtmosphereThick = 1.0f;
    float Exposure = 1.3f;
    float Turbidity = 2.0f;
};