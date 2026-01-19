#include "Pch.h"
#include "Vector3.h"
#include "Matrix4x4.h" // 実装のためにインクルード

namespace ACSU_Math
{
    Vector3 Vector3::RotateTowards(const Vector3& current, const Vector3& target, float maxRadiansDelta, float maxMagnitudeDelta)
    {
        float lenCurrent = current.magnitude();
        float lenTarget = target.magnitude();
        if (lenCurrent > 1.0e-5f && lenTarget > 1.0e-5f)
        {
            Vector3 from = current / lenCurrent;
            Vector3 to = target / lenTarget;
            float dot = Dot(from, to);
            dot = Mathf::Clamp(dot, -1.0f, 1.0f);
            float angle = std::acosf(dot);
            if (angle == 0.0f) return MoveTowards(current, target, maxMagnitudeDelta);

            float t = std::fmin(1.0f, maxRadiansDelta / angle);
            float sinAngle = std::sinf(angle);
            if (sinAngle < 0.001f) return MoveTowards(current, target, maxMagnitudeDelta);

            float coeff0 = std::sinf((1.0f - t) * angle) / sinAngle;
            float coeff1 = std::sinf(t * angle) / sinAngle;
            Vector3 newDir = from * coeff0 + to * coeff1;
            float newLen = Mathf::MoveTowards(lenCurrent, lenTarget, maxMagnitudeDelta);
            return newDir * newLen;
        }
        return MoveTowards(current, target, maxMagnitudeDelta);
    }

    Matrix4x4 Vector3::ToTranslateMatrix() const
    {
        Matrix4x4 m = Matrix4x4::identity();
        m.m30 = x; m.m31 = y; m.m32 = z;
        return m;
    }

    Matrix4x4 Vector3::ToScalingMatrix() const
    {
        Matrix4x4 m = Matrix4x4::identity();
        m.m00 = x; m.m01 = 0.0f; m.m02 = 0.0f; m.m03 = 0.0f;
        m.m10 = 0.0f; m.m11 = y; m.m12 = 0.0f; m.m13 = 0.0f;
        m.m20 = 0.0f; m.m21 = 0.0f; m.m22 = z; m.m23 = 0.0f;
        m.m30 = 0.0f; m.m31 = 0.0f; m.m32 = 0.0f; m.m33 = 1.0f;
        return m;
    }

    Matrix4x4 Vector3::ToRotationMatrix() const
    {
        float cx = cosf(x); float sx = sinf(x);
        float cy = cosf(y); float sy = sinf(y);
        float cz = cosf(z); float sz = sinf(z);
        Matrix4x4 m{};
        // Row 0
        m.m00 = cy * cz;
        m.m01 = sx * sy * cz + cx * sz;
        m.m02 = -cx * sy * cz + sx * sz;
        m.m03 = 0.0f;
        // Row 1
        m.m10 = -cy * sz;
        m.m11 = -sx * sy * sz + cx * cz;
        m.m12 = cx * sy * sz + sx * cz;
        m.m13 = 0.0f;
        // Row 2
        m.m20 = sy;
        m.m21 = -sx * cy;
        m.m22 = cx * cy;
        m.m23 = 0.0f;
        // Row 3
        m.m30 = 0.0f; m.m31 = 0.0f; m.m32 = 0.0f; m.m33 = 1.0f;
        return m;
    }

    Vector3::Vector3(const Vector2& v) : x(v.x), y(v.y), z(0.0f) {}

    Vector3::operator Vector2() const { return Vector2(x, y); }
    Vector2 Vector3::ToVector2() const { return Vector2(x, y); }
}