#include "Pch.h"

namespace ACSU_Math
{
    Vector2::Vector2(const Vector3& v) : x(v.x), y(v.y) {}

    Vector2::operator Vector3() const { return Vector3(x, y, 0.0f); }
    Vector3 Vector2::ToVector3() const { return Vector3(x, y, 0.0f); }
}