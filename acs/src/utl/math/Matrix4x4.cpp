#include "Pch.h"
#include "Matrix4x4.h"
#include "Quaternion.h" // Rotate実装に必要

namespace ACSU_Math
{
    Matrix4x4 Matrix4x4::identity()
    {
        Matrix4x4 m;
        m.m00 = 1; m.m01 = 0; m.m02 = 0; m.m03 = 0;
        m.m10 = 0; m.m11 = 1; m.m12 = 0; m.m13 = 0;
        m.m20 = 0; m.m21 = 0; m.m22 = 1; m.m23 = 0;
        m.m30 = 0; m.m31 = 0; m.m32 = 0; m.m33 = 1;
        return m;
    }

    Matrix4x4 Matrix4x4::zero()
    {
        Matrix4x4 m;
        for (int i = 0; i < 16; ++i) (&m.m00)[i] = 0.0f;
        return m;
    }

    Matrix4x4 Matrix4x4::Translate(const Vector3& v)
    {
        Matrix4x4 m = identity();
        m.m03 = v.x; m.m13 = v.y; m.m23 = v.z;
        return m;
    }

    Matrix4x4 Matrix4x4::Scale(const Vector3& v)
    {
        Matrix4x4 m = identity();
        m.m00 = v.x; m.m11 = v.y; m.m22 = v.z;
        return m;
    }

    Matrix4x4 Matrix4x4::Rotate(const Quaternion& q)
    {
        Quaternion n = q.normalized();
        float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
        float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
        float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;

        Matrix4x4 m;
        m.m00 = 1.0f - 2.0f * (yy + zz); m.m01 = 2.0f * (xy - wz);        m.m02 = 2.0f * (xz + wy);        m.m03 = 0.0f;
        m.m10 = 2.0f * (xy + wz);        m.m11 = 1.0f - 2.0f * (xx + zz); m.m12 = 2.0f * (yz - wx);        m.m13 = 0.0f;
        m.m20 = 2.0f * (xz - wy);        m.m21 = 2.0f * (yz + wx);        m.m22 = 1.0f - 2.0f * (xx + yy); m.m23 = 0.0f;
        m.m30 = 0.0f;                    m.m31 = 0.0f;                    m.m32 = 0.0f;                    m.m33 = 1.0f;
        return m;
    }

    Matrix4x4 Matrix4x4::MakeTransform(const Vector3& pos, const Quaternion& q, const Vector3& s)
    {
        Matrix4x4 m = Rotate(q);

        m.m00 *= s.x; m.m01 *= s.x; m.m02 *= s.x;
        m.m10 *= s.y; m.m11 *= s.y; m.m12 *= s.y;
        m.m20 *= s.z; m.m21 *= s.z; m.m22 *= s.z;

        m.m30 = pos.x;
        m.m31 = pos.y;
        m.m32 = pos.z;

        m.m03 = 0.0f; m.m13 = 0.0f; m.m23 = 0.0f; m.m33 = 1.0f;

        return m;
    }

    bool Matrix4x4::Decompose(Vector3& outPos, Quaternion& outRot, Vector3& outScale) const
    {
        // Translation (Row 3)
        outPos.x = m30;
        outPos.y = m31;
        outPos.z = m32;

        // Scale (各行ベクトルの長さ)
        Vector3 row0(m00, m01, m02);
        Vector3 row1(m10, m11, m12);
        Vector3 row2(m20, m21, m22);

        outScale.x = row0.magnitude();
        outScale.y = row1.magnitude();
        outScale.z = row2.magnitude();

        if (outScale.x < 1e-6f || outScale.y < 1e-6f || outScale.z < 1e-6f)
        {
            outRot = Quaternion::identity();
            return false;
        }

        Vector3 n0 = row0 / outScale.x;
        Vector3 n1 = row1 / outScale.y;
        Vector3 n2 = row2 / outScale.z;

        Matrix4x4 rotMat = Matrix4x4::identity();
        rotMat.m00 = n0.x; rotMat.m01 = n1.x; rotMat.m02 = n2.x;
        rotMat.m10 = n0.y; rotMat.m11 = n1.y; rotMat.m12 = n2.y;
        rotMat.m20 = n0.z; rotMat.m21 = n1.z; rotMat.m22 = n2.z;

        float trace = rotMat.m00 + rotMat.m11 + rotMat.m22;
        if (trace > 0.0f) {
            float s = 0.5f / std::sqrtf(trace + 1.0f);
            outRot.w = 0.25f / s;
            outRot.x = (rotMat.m21 - rotMat.m12) * s;
            outRot.y = (rotMat.m02 - rotMat.m20) * s;
            outRot.z = (rotMat.m10 - rotMat.m01) * s;
        }
        else {
            if (rotMat.m00 > rotMat.m11 && rotMat.m00 > rotMat.m22) {
                float s = 2.0f * std::sqrtf(1.0f + rotMat.m00 - rotMat.m11 - rotMat.m22);
                outRot.w = (rotMat.m21 - rotMat.m12) / s;
                outRot.x = 0.25f * s;
                outRot.y = (rotMat.m01 + rotMat.m10) / s;
                outRot.z = (rotMat.m02 + rotMat.m20) / s;
            }
            else if (rotMat.m11 > rotMat.m22) {
                float s = 2.0f * std::sqrtf(1.0f + rotMat.m11 - rotMat.m00 - rotMat.m22);
                outRot.w = (rotMat.m02 - rotMat.m20) / s;
                outRot.x = (rotMat.m01 + rotMat.m10) / s;
                outRot.y = 0.25f * s;
                outRot.z = (rotMat.m12 + rotMat.m21) / s;
            }
            else {
                float s = 2.0f * std::sqrtf(1.0f + rotMat.m22 - rotMat.m00 - rotMat.m11);
                outRot.w = (rotMat.m10 - rotMat.m01) / s;
                outRot.x = (rotMat.m02 + rotMat.m20) / s;
                outRot.y = (rotMat.m12 + rotMat.m21) / s;
                outRot.z = 0.25f * s;
            }
        }
        return true;
    }

    Matrix4x4 Matrix4x4::inverse() const
    {
        Matrix4x4 result;
        float det;

        float m00 = this->m00, m01 = this->m01, m02 = this->m02, m03 = this->m03;
        float m10 = this->m10, m11 = this->m11, m12 = this->m12, m13 = this->m13;
        float m20 = this->m20, m21 = this->m21, m22 = this->m22, m23 = this->m23;
        float m30 = this->m30, m31 = this->m31, m32 = this->m32, m33 = this->m33;

        float b00 = m00 * m11 - m01 * m10;
        float b01 = m00 * m12 - m02 * m10;
        float b02 = m00 * m13 - m03 * m10;
        float b03 = m01 * m12 - m02 * m11;
        float b04 = m01 * m13 - m03 * m11;
        float b05 = m02 * m13 - m03 * m12;
        float b06 = m20 * m31 - m21 * m30;
        float b07 = m20 * m32 - m22 * m30;
        float b08 = m20 * m33 - m23 * m30;
        float b09 = m21 * m32 - m22 * m31;
        float b10 = m21 * m33 - m23 * m31;
        float b11 = m22 * m33 - m23 * m32;

        det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;

        if (std::abs(det) < 1.0e-6f) return Matrix4x4::zero();

        float invDet = 1.0f / det;

        result.m00 = (m11 * b11 - m12 * b10 + m13 * b09) * invDet;
        result.m01 = (m02 * b10 - m01 * b11 - m03 * b09) * invDet;
        result.m02 = (m31 * b05 - m32 * b04 + m33 * b03) * invDet;
        result.m03 = (m22 * b04 - m21 * b05 - m23 * b03) * invDet;
        result.m10 = (m12 * b08 - m10 * b11 - m13 * b07) * invDet;
        result.m11 = (m00 * b11 - m02 * b08 + m03 * b07) * invDet;
        result.m12 = (m32 * b02 - m30 * b05 - m33 * b01) * invDet;
        result.m13 = (m20 * b05 - m22 * b02 + m23 * b01) * invDet;
        result.m20 = (m10 * b10 - m11 * b08 + m13 * b06) * invDet;
        result.m21 = (m01 * b08 - m00 * b10 - m03 * b06) * invDet;
        result.m22 = (m30 * b04 - m31 * b02 + m33 * b00) * invDet;
        result.m23 = (m21 * b02 - m20 * b04 - m23 * b00) * invDet;
        result.m30 = (m11 * b07 - m10 * b09 - m12 * b06) * invDet;
        result.m31 = (m00 * b09 - m01 * b07 + m02 * b06) * invDet;
        result.m32 = (m31 * b01 - m30 * b03 - m32 * b00) * invDet;
        result.m33 = (m20 * b03 - m21 * b01 + m22 * b00) * invDet;

        return result;
    }

    Matrix4x4 operator*(const Matrix4x4& a, const Matrix4x4& b)
    {
        Matrix4x4 r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a(row, k) * b(k, col);
                }
                r(row, col) = sum;
            }
        }
        return r;
    }
}