#include "ACSM_DxLib.h"

VECTOR VGet(VECTOR_D v)
{
    return VGet((float)v.x, (float)v.y, (float)v.z);
}

VECTOR_D VGetD(VECTOR v)
{
    return VGetD((double)v.x, (double)v.y, (double)v.z);
}

const VECTOR operator+(const VECTOR& v1, const VECTOR& v2)
{
    return VAdd(v1, v2);
}

const VECTOR_D operator+(const VECTOR_D& v1, const VECTOR_D& v2)
{
    return VAddD(v1, v2);
}

VECTOR& operator+=(VECTOR& v1, const VECTOR& v2)
{
    return v1 = VAdd(v1, v2);
}

VECTOR_D& operator+=(VECTOR_D& v1, const VECTOR_D& v2)
{
    return v1 = VAddD(v1, v2);
}

const VECTOR operator-(const VECTOR& v1, const VECTOR& v2)
{
    return VSub(v1, v2);
}

const VECTOR_D operator-(const VECTOR_D& v1, const VECTOR_D& v2)
{
    return VSubD(v1, v2);
}

VECTOR& operator-=(VECTOR& v1, const VECTOR& v2)
{
    return v1 = VSub(v1, v2);
}

VECTOR_D& operator-=(VECTOR_D& v1, const VECTOR_D& v2)
{
    return v1 = VSubD(v1, v2);
}

const VECTOR operator*(const VECTOR& v1, const float& scale)
{
    return VScale(v1, scale);
}

const VECTOR_D operator*(const VECTOR_D& v1, const double& scale)
{
    return VScaleD(v1, scale);
}

const VECTOR operator*(const float& scale, const VECTOR& v1)
{
    return VScale(v1, scale);
}

const VECTOR_D operator*(const double& scale, const VECTOR_D& v1)
{
    return VScaleD(v1, scale);
}

const VECTOR operator*(const VECTOR& v1, const VECTOR& v2)
{
    return VECTOR(
        v1.x * v2.x,
        v1.y * v2.y,
        v1.z * v2.z);
}

const VECTOR_D operator*(const VECTOR_D& v1, const VECTOR_D& v2)
{
    return VECTOR_D(
        v1.x * v2.x,
        v1.y * v2.y,
        v1.z * v2.z);
}

const VECTOR operator/(const VECTOR& v1, const float& scale)
{
    return VScale(v1, 1.0f / scale);
}

const VECTOR_D operator/(const VECTOR_D& v1, const double& scale)
{
    return VScaleD(v1, 1.0f / scale);
}

VECTOR& operator*=(VECTOR& v1, const float& scale)
{
    return v1 = VScale(v1, scale);
}

VECTOR_D& operator*=(VECTOR_D& v1, const double& scale)
{
    return v1 = VScaleD(v1, scale);
}

VECTOR& operator*=(const float& scale, VECTOR& v1)
{
    return v1 = VScale(v1, scale);
}

VECTOR_D& operator*=(const double& scale, VECTOR_D& v1)
{
    return v1 = VScaleD(v1, scale);
}

VECTOR& operator/=(VECTOR& v1, const float& scale)
{
    return v1 = VScale(v1, 1.0f / scale);
}

VECTOR_D& operator/=(VECTOR_D& v1, const double& scale)
{
    return v1 = VScaleD(v1, 1.0f / scale);
}

const MATRIX operator+(const MATRIX& m1, const MATRIX& m2)
{
    return MAdd(m1, m2);
}

const MATRIX_D operator+(const MATRIX_D& m1, const MATRIX_D& m2)
{
    return MAddD(m1, m2);
}

MATRIX& operator+=(MATRIX& m1, const MATRIX& m2)
{
    return m1 = MAdd(m1, m2);
}

MATRIX_D& operator+=(MATRIX_D& m1, const MATRIX_D& m2)
{
    return m1 = MAddD(m1, m2);
}

const MATRIX operator*(const MATRIX& m1, const MATRIX& m2)
{
    return MMult(m1, m2);
}

const MATRIX_D operator*(const MATRIX_D& m1, const MATRIX_D& m2)
{
    return MMultD(m1, m2);
}

MATRIX& operator*=(MATRIX& m1, const MATRIX& m2)
{
    return m1 = MMult(m1, m2);
}

MATRIX_D& operator*=(MATRIX_D& m1, const MATRIX_D& m2)
{
    return m1 = MMultD(m1, m2);
}

const VECTOR operator*(const VECTOR& v, const MATRIX& m1)
{
    return VTransform(v, m1);
}

const VECTOR_D operator*(const VECTOR_D& v, const MATRIX_D& m1)
{
    return VTransformD(v, m1);
}

VECTOR& operator*=(VECTOR& v, const MATRIX& m1)
{
    return v = VTransform(v, m1);
}

VECTOR_D& operator*=(VECTOR_D& v, const MATRIX_D& m1)
{
    return v = VTransformD(v, m1);
}
