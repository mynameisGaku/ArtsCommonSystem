#pragma once
#include "Vector3.h"
#include "Ray.h"

namespace ACSU_Math
{
    struct Plane
    {
        Vector3 normal;
        float distance;

        Plane() : normal(Vector3::up()), distance(0.0f) {}
        Plane(const Vector3& inNormal, const Vector3& inPoint) : normal(inNormal.normalized()), distance(0.0f) {
            distance = -Vector3::Dot(normal, inPoint);
        }

        bool Raycast(const Ray& ray, float& enter) const
        {
            float denom = Vector3::Dot(normal, ray.direction);
            if (Mathf::Abs(denom) < 1.0e-15f) {
                enter = 0.0f;
                return false;
            }
            enter = -(Vector3::Dot(normal, ray.origin) + distance) / denom;
            return enter >= 0.0f;
        }
    };
}