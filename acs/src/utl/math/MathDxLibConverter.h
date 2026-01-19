#pragma once
#include "DxLib.h"
#include "Matrix4x4.h"
#include "Vector3.h"
#include "Vector2.h"
#include "Color.h"

namespace ACSU_Math
{
    struct MathDxLibConverter
    {
        static inline DxLib::MATRIX ToDxMatrix(const Matrix4x4& m)
        {
            DxLib::MATRIX mat;
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) mat.m[i][j] = m(i, j);
            return mat;
        }
        static inline Matrix4x4 ToMatrix4x4(const DxLib::MATRIX& mat)
        {
            Matrix4x4 m;
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m(i, j) = mat.m[i][j];
            return m;
        }

        static inline DxLib::VECTOR ToDxVector(const Vector3& v)
        {
            DxLib::VECTOR vec; vec.x = v.x; vec.y = v.y; vec.z = v.z; return vec;
        }
        static inline Vector3 ToVector3(const DxLib::VECTOR& vec)
        {
            return Vector3(vec.x, vec.y, vec.z);
        }

        static inline DxLib::COLOR_F ToDxColorF(const Color& c)
        {
            DxLib::COLOR_F col; col.r = c.r; col.g = c.g; col.b = c.b; col.a = c.a; return col;
        }
        static inline Color ToColor(const DxLib::COLOR_F& col)
        {
            return Color(col.r, col.g, col.b, col.a);
        }

        template<typename T_OUT, typename T_IN>
        static T_OUT Convert(const T_IN& input);

        template<> static DxLib::MATRIX Convert(const Matrix4x4& in) { return ToDxMatrix(in); }
        template<> static Matrix4x4 Convert(const DxLib::MATRIX& in) { return ToMatrix4x4(in); }
        template<> static DxLib::VECTOR Convert(const Vector3& in) { return ToDxVector(in); }
        template<> static Vector3 Convert(const DxLib::VECTOR& in) { return ToVector3(in); }
    };
}