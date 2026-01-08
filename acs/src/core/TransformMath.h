#pragma once

#include <Pch.h>

namespace ACSU_Math
{
    inline Mat4 MakeTRS(const Vec3& position, const Quat& rotation, const Vec3& scale)
    {
        return Mat4::CreateTranslation(position) * rotation.ToMat4() * Mat4::CreateScale(scale);
    }

    inline Mat4 InverseMatrix(const Mat4& m)
    {
        return m.Inverted();
    }

    inline void DecomposeTRS(const Mat4& m, Vec3& outPosition, Quat& outRotation, Vec3& outScale)
    {
        m.DecomposeTRS(outPosition, outRotation, outScale);
    }
}
