#include "Pch.h"
#include "Quaternion.h"
#include "Matrix4x4.h" // ToMatrixの実装に必要

namespace ACSU_Math
{
    Quaternion Quaternion::Euler(float x, float y, float z)
    {
        float radX = x * Mathf::Deg2Rad * 0.5f;
        float radY = y * Mathf::Deg2Rad * 0.5f;
        float radZ = z * Mathf::Deg2Rad * 0.5f;

        float sinX = std::sinf(radX); float cosX = std::cosf(radX);
        float sinY = std::sinf(radY); float cosY = std::cosf(radY);
        float sinZ = std::sinf(radZ); float cosZ = std::cosf(radZ);

        return Quaternion(
            cosY * sinX * cosZ + sinY * cosX * sinZ,
            sinY * cosX * cosZ - cosY * sinX * sinZ,
            cosY * cosX * sinZ - sinY * sinX * cosZ,
            cosY * cosX * cosZ + sinY * sinX * sinZ
        );
    }

    Quaternion Quaternion::AngleAxis(float angle, const Vector3& axis)
    {
        Vector3 n = axis.normalized();
        float rad = angle * Mathf::Deg2Rad * 0.5f;
        float s = std::sinf(rad);
        return Quaternion(n.x * s, n.y * s, n.z * s, std::cosf(rad));
    }

    Quaternion Quaternion::LookRotation(const Vector3& forward, const Vector3& upwards)
    {
        Vector3 f = forward.normalized();
        Vector3 r = Vector3::Cross(upwards, f).normalized();
        Vector3 u = Vector3::Cross(f, r);

        // 回転行列の成分からQuaternionを復元するロジック (Matrix4x4::ToRotationMatrixに近い処理)
        float m00 = r.x, m01 = r.y, m02 = r.z;
        float m10 = u.x, m11 = u.y, m12 = u.z;
        float m20 = f.x, m21 = f.y, m22 = f.z;

        float trace = m00 + m11 + m22;
        Quaternion q;
        if (trace > 0.0f) {
            float s = 0.5f / std::sqrtf(trace + 1.0f);
            q.w = 0.25f / s;
            q.x = (m21 - m12) * s;
            q.y = (m02 - m20) * s;
            q.z = (m10 - m01) * s;
        }
        else {
            if (m00 > m11 && m00 > m22) {
                float s = 2.0f * std::sqrtf(1.0f + m00 - m11 - m22);
                q.w = (m21 - m12) / s;
                q.x = 0.25f * s;
                q.y = (m01 + m10) / s;
                q.z = (m02 + m20) / s;
            }
            else if (m11 > m22) {
                float s = 2.0f * std::sqrtf(1.0f + m11 - m00 - m22);
                q.w = (m02 - m20) / s;
                q.x = (m01 + m10) / s;
                q.y = 0.25f * s;
                q.z = (m12 + m21) / s;
            }
            else {
                float s = 2.0f * std::sqrtf(1.0f + m22 - m00 - m11);
                q.w = (m10 - m01) / s;
                q.x = (m02 + m20) / s;
                q.y = (m12 + m21) / s;
                q.z = 0.25f * s;
            }
        }
        return q;
    }

    Quaternion Quaternion::FromToRotation(const Vector3& fromDirection, const Vector3& toDirection)
    {
        Vector3 from = fromDirection.normalized();
        Vector3 to = toDirection.normalized();

        float dot = Vector3::Dot(from, to);

        if (dot >= 1.0f - 1.0e-6f)
        {
            return Quaternion::identity();
        }
        else if (dot <= -1.0f + 1.0e-6f)
        {
            // 真逆の場合、任意の垂直軸で180度回転
            Vector3 axis = Vector3::Cross(Vector3::right(), from);
            if (axis.sqrMagnitude() < 1.0e-6f)
            {
                axis = Vector3::Cross(Vector3::up(), from);
            }
            return Quaternion::AngleAxis(180.0f, axis);
        }
        else
        {
            float s = std::sqrtf((1.0f + dot) * 2.0f);
            float invs = 1.0f / s;
            Vector3 c = Vector3::Cross(from, to);
            return Quaternion(c.x * invs, c.y * invs, c.z * invs, s * 0.5f).normalized();
        }
    }

    Quaternion Quaternion::SlerpUnclamped(const Quaternion& a, const Quaternion& b, float t)
    {
        float dot = Dot(a, b);
        Quaternion tempB = b;

        if (dot < 0.0f)
        {
            dot = -dot;
            tempB.x = -tempB.x; tempB.y = -tempB.y; tempB.z = -tempB.z; tempB.w = -tempB.w;
        }

        if (dot > 0.9995f)
        {
            // Lerp logic
            float oneMinusT = 1.0f - t;
            return Quaternion(
                a.x * oneMinusT + tempB.x * t,
                a.y * oneMinusT + tempB.y * t,
                a.z * oneMinusT + tempB.z * t,
                a.w * oneMinusT + tempB.w * t
            ).normalized();
        }

        float theta = std::acosf(dot);
        float sinTheta = std::sinf(theta);

        float w1 = std::sinf((1.0f - t) * theta) / sinTheta;
        float w2 = std::sinf(t * theta) / sinTheta;

        return Quaternion(
            a.x * w1 + tempB.x * w2,
            a.y * w1 + tempB.y * w2,
            a.z * w1 + tempB.z * w2,
            a.w * w1 + tempB.w * w2
        );
    }

    Matrix4x4 Quaternion::ToMatrix() const
    {
        return Matrix4x4::Rotate(*this);
    }

    Quaternion Quaternion::Inverse(const Quaternion& rotation)
    {
        float n2 = rotation.x * rotation.x +
            rotation.y * rotation.y +
            rotation.z * rotation.z +
            rotation.w * rotation.w;

        if (n2 < 1.0e-6f)
        {
            return Quaternion::identity();
        }

        float invN2 = 1.0f / n2;

        return Quaternion(
            -rotation.x * invN2,
            -rotation.y * invN2,
            -rotation.z * invN2,
            rotation.w * invN2
        );
    }
}