#pragma once

#include <Pch.h>

struct TransformTRS
{
public:
    ACSU_Math::Vec3 position = ACSU_Math::Vec3::Zero();
    ACSU_Math::Quat rotation = ACSU_Math::Quat::Identity();
    ACSU_Math::Vec3 scale = ACSU_Math::Vec3::One();
};
