#pragma once

#include <Pch.h>

struct TransformTRS
{
public:
    ACSU_Math::Vector3 position = ACSU_Math::Vector3::zero();
    ACSU_Math::Quaternion rotation = ACSU_Math::Quaternion::identity();
    ACSU_Math::Vector3 scale = ACSU_Math::Vector3::one();
	ACSU_Math::Vector3 forward = ACSU_Math::Vector3::forward();
	ACSU_Math::Vector3 up = ACSU_Math::Vector3::up();
};
