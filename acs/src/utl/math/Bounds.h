#pragma once
#include "Vector3.h"

namespace ACSU_Math
{
    struct Bounds
    {
        Vector3 center;
        Vector3 size;

        Bounds() : center(Vector3::zero()), size(Vector3::zero()) {}
        Bounds(const Vector3& _center, const Vector3& _size) : center(_center), size(_size) {}

        inline Vector3 extents() const { return size * 0.5f; }
        inline Vector3 min() const { return center - extents(); }
        inline Vector3 max() const { return center + extents(); }

        inline bool Contains(const Vector3& point) const {
            Vector3 mn = min();
            Vector3 mx = max();
            return (point.x >= mn.x && point.x <= mx.x) &&
                (point.y >= mn.y && point.y <= mx.y) &&
                (point.z >= mn.z && point.z <= mx.z);
        }

        inline void Encapsulate(const Vector3& point) {
            Vector3 mn = min();
            Vector3 mx = max();
            mn.x = Mathf::Min(mn.x, point.x); mn.y = Mathf::Min(mn.y, point.y); mn.z = Mathf::Min(mn.z, point.z);
            mx.x = Mathf::Max(mx.x, point.x); mx.y = Mathf::Max(mx.y, point.y); mx.z = Mathf::Max(mx.z, point.z);
            center = (mn + mx) * 0.5f;
            size = (mx - mn);
        }
    };
}