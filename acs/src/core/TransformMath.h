#pragma once

#include <Pch.h>

namespace ACSU_Math
{
    inline Matrix4x4 MakeTRS(const Vector3& position, const Quaternion& rotation, const Vector3& scale)
    {
        return Matrix4x4::Translate(position) * Matrix4x4::Rotate(rotation) * Matrix4x4::Scale(scale);
    }

    inline Matrix4x4 InverseMatrix(const Matrix4x4& m)
    {
        return Matrix4x4::Inverse(m);
    }

    inline void DecomposeTRS(const Matrix4x4& m, Vector3& outPosition, Quaternion& outRotation, Vector3& outScale)
    {
        outPosition = Vector3(m.m03, m.m13, m.m23);

        Vector3 col0(m.m00, m.m10, m.m20);
        Vector3 col1(m.m01, m.m11, m.m21);
        Vector3 col2(m.m02, m.m12, m.m22);
        outScale.x = col0.magnitude();
        outScale.y = col1.magnitude();
        outScale.z = col2.magnitude();

        Matrix4x4 rotMat = Matrix4x4::identity();
        if (outScale.x != 0.0f) { rotMat.m00 = m.m00 / outScale.x; rotMat.m10 = m.m10 / outScale.x; rotMat.m20 = m.m20 / outScale.x; }
        if (outScale.y != 0.0f) { rotMat.m01 = m.m01 / outScale.y; rotMat.m11 = m.m11 / outScale.y; rotMat.m21 = m.m21 / outScale.y; }
        if (outScale.z != 0.0f) { rotMat.m02 = m.m02 / outScale.z; rotMat.m12 = m.m12 / outScale.z; rotMat.m22 = m.m22 / outScale.z; }

        float trace = rotMat.m00 + rotMat.m11 + rotMat.m22;
        if (trace > 0.0f)
        {
            float s = std::sqrtf(trace + 1.0f) * 2.0f;
            outRotation.w = 0.25f * s;
            outRotation.x = (rotMat.m21 - rotMat.m12) / s;
            outRotation.y = (rotMat.m02 - rotMat.m20) / s;
            outRotation.z = (rotMat.m10 - rotMat.m01) / s;
        }
        else if (rotMat.m00 > rotMat.m11 && rotMat.m00 > rotMat.m22)
        {
            float s = std::sqrtf(1.0f + rotMat.m00 - rotMat.m11 - rotMat.m22) * 2.0f;
            outRotation.w = (rotMat.m21 - rotMat.m12) / s;
            outRotation.x = 0.25f * s;
            outRotation.y = (rotMat.m01 + rotMat.m10) / s;
            outRotation.z = (rotMat.m02 + rotMat.m20) / s;
        }
        else if (rotMat.m11 > rotMat.m22)
        {
            float s = std::sqrtf(1.0f + rotMat.m11 - rotMat.m00 - rotMat.m22) * 2.0f;
            outRotation.w = (rotMat.m02 - rotMat.m20) / s;
            outRotation.x = (rotMat.m01 + rotMat.m10) / s;
            outRotation.y = 0.25f * s;
            outRotation.z = (rotMat.m12 + rotMat.m21) / s;
        }
        else
        {
            float s = std::sqrtf(1.0f + rotMat.m22 - rotMat.m00 - rotMat.m11) * 2.0f;
            outRotation.w = (rotMat.m10 - rotMat.m01) / s;
            outRotation.x = (rotMat.m02 + rotMat.m20) / s;
            outRotation.y = (rotMat.m12 + rotMat.m21) / s;
            outRotation.z = 0.25f * s;
        }
        outRotation = outRotation.normalized();
    }
}
