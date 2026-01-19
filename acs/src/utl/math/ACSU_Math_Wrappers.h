#pragma once
#include "Matrix4x4.h"
#include "Quaternion.h"
#include "Vector3.h"

namespace ACSU_Math
{
    // トランスフォームを生成
    static inline Matrix4x4 MakeTransform(const Vector3& pos, const Quaternion& rot, const Vector3& scale)
    {
        return Matrix4x4::MakeTransform(pos, rot, scale);
    }

    // トランスフォームを各要素に分解
    static inline void DecomposeTransform(const Matrix4x4& mat, Vector3& outPos, Quaternion& outRot, Vector3& outScale)
    {
        mat.Decompose(outPos, outRot, outScale);
    }

    // 逆行列を返す
    static inline Matrix4x4 InverseMatrix(const Matrix4x4& mat)
    {
        return mat.inverse();
    }
}