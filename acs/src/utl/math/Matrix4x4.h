#pragma once
#include "Mathf.h"
#include "Vector3.h"
#include "Vector4.h"

namespace ACSU_Math
{
    struct Quaternion; // 前方宣言

    struct Matrix4x4
    {
        Matrix4x4() :
            m00(1.0f), m01(0.0f), m02(0.0f), m03(0.0f),
            m10(0.0f), m11(1.0f), m12(0.0f), m13(0.0f),
            m20(0.0f), m21(0.0f), m22(1.0f), m23(0.0f),
            m30(0.0f), m31(0.0f), m32(0.0f), m33(1.0f) {}

        float m00, m01, m02, m03;
        float m10, m11, m12, m13;
        float m20, m21, m22, m23;
        float m30, m31, m32, m33;

        static Matrix4x4 identity();
        static Matrix4x4 zero();

        inline float& operator()(int row, int col) { return (&m00)[row * 4 + col]; }
        inline const float& operator()(int row, int col) const { return (&m00)[row * 4 + col]; }

        static Matrix4x4 Translate(const Vector3& v);
        static Matrix4x4 Scale(const Vector3& v);
        static Matrix4x4 Rotate(const Quaternion& q); // 実装はcppへ

        static Matrix4x4 MakeTransform(const Vector3& pos, const Quaternion& q, const Vector3& s);
        bool Decompose(Vector3& outPos, Quaternion& outRot, Vector3& outScale) const;

        inline Vector3 MultiplyPoint(const Vector3& v) const {
            float x = m00 * v.x + m01 * v.y + m02 * v.z + m03;
            float y = m10 * v.x + m11 * v.y + m12 * v.z + m13;
            float z = m20 * v.x + m21 * v.y + m22 * v.z + m23;
            float w = m30 * v.x + m31 * v.y + m32 * v.z + m33;
            if (Mathf::Abs(w) > 1.0e-5f) {
                float invW = 1.0f / w;
                return Vector3(x * invW, y * invW, z * invW);
            }
            return Vector3::zero();
        }

        inline Vector3 MultiplyPoint3x4(const Vector3& v) const {
            return Vector3(
                m00 * v.x + m01 * v.y + m02 * v.z + m03,
                m10 * v.x + m11 * v.y + m12 * v.z + m13,
                m20 * v.x + m21 * v.y + m22 * v.z + m23
            );
        }

        Matrix4x4 inverse() const; // 逆行列

        friend Matrix4x4 operator*(const Matrix4x4& a, const Matrix4x4& b);

        operator DxLib::MATRIX() const
        {
            DxLib::MATRIX mat;
            mat.m[0][0] = m00; mat.m[0][1] = m01; mat.m[0][2] = m02; mat.m[0][3] = m03;
            mat.m[1][0] = m10; mat.m[1][1] = m11; mat.m[1][2] = m12; mat.m[1][3] = m13;
            mat.m[2][0] = m20; mat.m[2][1] = m21; mat.m[2][2] = m22; mat.m[2][3] = m23;
            mat.m[3][0] = m30; mat.m[3][1] = m31; mat.m[3][2] = m32; mat.m[3][3] = m33;
            return mat;
        }

        Matrix4x4(const DxLib::MATRIX& mat)
        {
            m00 = mat.m[0][0]; m01 = mat.m[0][1]; m02 = mat.m[0][2]; m03 = mat.m[0][3];
            m10 = mat.m[1][0]; m11 = mat.m[1][1]; m12 = mat.m[1][2]; m13 = mat.m[1][3];
            m20 = mat.m[2][0]; m21 = mat.m[2][1]; m22 = mat.m[2][2]; m23 = mat.m[2][3];
            m30 = mat.m[3][0]; m31 = mat.m[3][1]; m32 = mat.m[3][2]; m33 = mat.m[3][3];
        }
    };
}