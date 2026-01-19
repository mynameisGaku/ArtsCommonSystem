#pragma once
#include "Vector3.h"

namespace ACSU_Math
{
    struct Ray
    {
        Vector3 origin;
        Vector3 direction;

        Ray() : origin(Vector3::zero()), direction(Vector3::forward()) {}
        Ray(const Vector3& _origin, const Vector3& _direction) : origin(_origin), direction(_direction.normalized()) {}

        inline Vector3 GetPoint(float distance) const {
            return origin + direction * distance;
        }
    };
}