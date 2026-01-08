#pragma once

#include <Pch.h>

#if __has_include(<DxLib.h>)
#define ACSU_HAS_DXLIB 1
#else
#define ACSU_HAS_DXLIB 0
#endif

namespace ACSU_Math
{
    inline float Abs(float v)
    {
        return std::fabs(v);
    }

    inline float Sqrt(float v)
    {
        return std::sqrt(v);
    }

    inline bool NearlyEqual(float a, float b, float eps = 1e-6f)
    {
        return Abs(a - b) <= eps;
    }

    struct Vec2
    {
    public:
        float x;
        float y;

        Vec2()
            : x(0.0f), y(0.0f)
        {
        }

        Vec2(float _x, float _y)
            : x(_x), y(_y)
        {
        }

        static Vec2 Zero()
        {
            return Vec2(0.0f, 0.0f);
        }

        static Vec2 One()
        {
            return Vec2(1.0f, 1.0f);
        }

#if ACSU_HAS_DXLIB
        Vec2(const VECTOR& v)
            : x(v.x), y(v.y)
        {
        }

        Vec2& operator=(const VECTOR& v)
        {
            x = v.x;
            y = v.y;
            return *this;
        }

        operator VECTOR() const
        {
            VECTOR v;
            v.x = x;
            v.y = y;
            v.z = 0.0f;
            return v;
        }
#endif

        Vec2 operator+(const Vec2& r) const { return Vec2(x + r.x, y + r.y); }
        Vec2 operator-(const Vec2& r) const { return Vec2(x - r.x, y - r.y); }
        Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
        Vec2 operator/(float s) const { return Vec2(x / s, y / s); }

        Vec2& operator+=(const Vec2& r) { x += r.x; y += r.y; return *this; }
        Vec2& operator-=(const Vec2& r) { x -= r.x; y -= r.y; return *this; }
        Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
        Vec2& operator/=(float s) { x /= s; y /= s; return *this; }
    };

    struct Vec3
    {
    public:
        float x;
        float y;
        float z;

        Vec3()
            : x(0.0f), y(0.0f), z(0.0f)
        {
        }

        Vec3(float _x, float _y, float _z)
            : x(_x), y(_y), z(_z)
        {
        }

        static Vec3 Zero()
        {
            return Vec3(0.0f, 0.0f, 0.0f);
        }

        static Vec3 One()
        {
            return Vec3(1.0f, 1.0f, 1.0f);
        }

#if ACSU_HAS_DXLIB
        Vec3(const VECTOR& v)
            : x(v.x), y(v.y), z(v.z)
        {
        }

        Vec3& operator=(const VECTOR& v)
        {
            x = v.x;
            y = v.y;
            z = v.z;
            return *this;
        }

        operator VECTOR() const
        {
            VECTOR v;
            v.x = x;
            v.y = y;
            v.z = z;
            return v;
        }
#endif

        float Length() const
        {
            return Sqrt(x * x + y * y + z * z);
        }

        Vec3 operator+(const Vec3& r) const { return Vec3(x + r.x, y + r.y, z + r.z); }
        Vec3 operator-(const Vec3& r) const { return Vec3(x - r.x, y - r.y, z - r.z); }
        Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
        Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }

        Vec3& operator+=(const Vec3& r) { x += r.x; y += r.y; z += r.z; return *this; }
        Vec3& operator-=(const Vec3& r) { x -= r.x; y -= r.y; z -= r.z; return *this; }
        Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
        Vec3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    };

    struct Mat4;

    struct Quat
    {
    public:
        float x;
        float y;
        float z;
        float w;

        Quat()
            : x(0.0f), y(0.0f), z(0.0f), w(1.0f)
        {
        }

        Quat(float _x, float _y, float _z, float _w)
            : x(_x), y(_y), z(_z), w(_w)
        {
        }

        static Quat Identity()
        {
            return Quat(0.0f, 0.0f, 0.0f, 1.0f);
        }

        Quat Normalized(float eps = 1e-6f) const
        {
            float len2 = x * x + y * y + z * z + w * w;
            if (len2 < eps * eps)
            {
                return Identity();
            }

            float invLen = 1.0f / Sqrt(len2);
            return Quat(x * invLen, y * invLen, z * invLen, w * invLen);
        }

        static Quat FromRotationMatrix3x3(
            float m00, float m01, float m02,
            float m10, float m11, float m12,
            float m20, float m21, float m22
        )
        {
            float trace = m00 + m11 + m22;

            float qx = 0.0f;
            float qy = 0.0f;
            float qz = 0.0f;
            float qw = 1.0f;

            if (trace > 0.0f)
            {
                float s = Sqrt(trace + 1.0f) * 2.0f;
                qw = 0.25f * s;
                qx = (m21 - m12) / s;
                qy = (m02 - m20) / s;
                qz = (m10 - m01) / s;
            }
            else if (m00 > m11 && m00 > m22)
            {
                float s = Sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                qw = (m21 - m12) / s;
                qx = 0.25f * s;
                qy = (m01 + m10) / s;
                qz = (m02 + m20) / s;
            }
            else if (m11 > m22)
            {
                float s = Sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                qw = (m02 - m20) / s;
                qx = (m01 + m10) / s;
                qy = 0.25f * s;
                qz = (m12 + m21) / s;
            }
            else
            {
                float s = Sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                qw = (m10 - m01) / s;
                qx = (m02 + m20) / s;
                qy = (m12 + m21) / s;
                qz = 0.25f * s;
            }

            return Quat(qx, qy, qz, qw).Normalized();
        }

        ACSU_Math::Vec3 RotateVector(const ACSU_Math::Vec3& v) const
        {
            Quat q = Normalized();

            Quat qv(v.x, v.y, v.z, 0.0f);
            Quat inv(-q.x, -q.y, -q.z, q.w);

            Quat a(
                q.w * qv.x + q.x * qv.w + q.y * qv.z - q.z * qv.y,
                q.w * qv.y - q.x * qv.z + q.y * qv.w + q.z * qv.x,
                q.w * qv.z + q.x * qv.y - q.y * qv.x + q.z * qv.w,
                q.w * qv.w - q.x * qv.x - q.y * qv.y - q.z * qv.z
            );

            Quat r(
                a.w * inv.x + a.x * inv.w + a.y * inv.z - a.z * inv.y,
                a.w * inv.y - a.x * inv.z + a.y * inv.w + a.z * inv.x,
                a.w * inv.z + a.x * inv.y - a.y * inv.x + a.z * inv.w,
                a.w * inv.w - a.x * inv.x - a.y * inv.y - a.z * inv.z
            );

            return ACSU_Math::Vec3(r.x, r.y, r.z);
        }
        Mat4 ToMat4() const;
    };

    struct Mat4
    {
    public:
        float m[4][4];

        Mat4()
        {
            SetIdentity();
        }

        static Mat4 Identity()
        {
            Mat4 r;
            r.SetIdentity();
            return r;
        }

        void SetIdentity()
        {
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    m[r][c] = (r == c) ? 1.0f : 0.0f;
                }
            }
        }

#if ACSU_HAS_DXLIB
        Mat4(const MATRIX& src)
        {
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    m[r][c] = src.m[r][c];
                }
            }
        }

        Mat4& operator=(const MATRIX& src)
        {
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    m[r][c] = src.m[r][c];
                }
            }
            return *this;
        }

        operator MATRIX() const
        {
            MATRIX r;
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    r.m[row][col] = m[row][col];
                }
            }
            return r;
        }
#endif

        static Mat4 CreateTranslation(const Vec3& t)
        {
            Mat4 r = Identity();
            r.m[3][0] = t.x;
            r.m[3][1] = t.y;
            r.m[3][2] = t.z;
            return r;
        }

        static Mat4 CreateScale(const Vec3& s)
        {
            Mat4 r = Identity();
            r.m[0][0] = s.x;
            r.m[1][1] = s.y;
            r.m[2][2] = s.z;
            return r;
        }

        static Mat4 CreateFromQuaternion(const Quat& qIn)
        {
            Quat q = qIn.Normalized();

            float xx = q.x * q.x;
            float yy = q.y * q.y;
            float zz = q.z * q.z;
            float xy = q.x * q.y;
            float xz = q.x * q.z;
            float yz = q.y * q.z;
            float wx = q.w * q.x;
            float wy = q.w * q.y;
            float wz = q.w * q.z;

            Mat4 r = Identity();

            r.m[0][0] = 1.0f - 2.0f * (yy + zz);
            r.m[0][1] = 2.0f * (xy + wz);
            r.m[0][2] = 2.0f * (xz - wy);

            r.m[1][0] = 2.0f * (xy - wz);
            r.m[1][1] = 1.0f - 2.0f * (xx + zz);
            r.m[1][2] = 2.0f * (yz + wx);

            r.m[2][0] = 2.0f * (xz + wy);
            r.m[2][1] = 2.0f * (yz - wx);
            r.m[2][2] = 1.0f - 2.0f * (xx + yy);

            return r;
        }

        Mat4 operator*(const Mat4& b) const
        {
            Mat4 r;

            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        sum += m[i][k] * b.m[k][j];
                    }
                    r.m[i][j] = sum;
                }
            }

            return r;
        }

        bool Inverse(Mat4& out) const
        {
            float a[4][8];

            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    a[r][c] = m[r][c];
                }
                for (int c = 0; c < 4; ++c)
                {
                    a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
                }
            }

            for (int i = 0; i < 4; ++i)
            {
                int pivot = i;
                float maxAbs = Abs(a[i][i]);

                for (int r = i + 1; r < 4; ++r)
                {
                    float v = Abs(a[r][i]);
                    if (v > maxAbs)
                    {
                        maxAbs = v;
                        pivot = r;
                    }
                }

                if (maxAbs < 1e-8f)
                {
                    return false;
                }

                if (pivot != i)
                {
                    for (int c = 0; c < 8; ++c)
                    {
                        float tmp = a[i][c];
                        a[i][c] = a[pivot][c];
                        a[pivot][c] = tmp;
                    }
                }

                float invPivot = 1.0f / a[i][i];
                for (int c = 0; c < 8; ++c)
                {
                    a[i][c] *= invPivot;
                }

                for (int r = 0; r < 4; ++r)
                {
                    if (r == i)
                    {
                        continue;
                    }

                    float factor = a[r][i];
                    for (int c = 0; c < 8; ++c)
                    {
                        a[r][c] -= factor * a[i][c];
                    }
                }
            }

            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    out.m[r][c] = a[r][4 + c];
                }
            }

            return true;
        }

        Mat4 Inverted() const
        {
            Mat4 inv;
            if (!Inverse(inv))
            {
                return Identity();
            }
            return inv;
        }

        bool DecomposeTRS(Vec3& outPosition, Quat& outRotation, Vec3& outScale) const
        {
            outPosition = Vec3(m[3][0], m[3][1], m[3][2]);

            Vec3 r0(m[0][0], m[0][1], m[0][2]);
            Vec3 r1(m[1][0], m[1][1], m[1][2]);
            Vec3 r2(m[2][0], m[2][1], m[2][2]);

            float sx = r0.Length();
            float sy = r1.Length();
            float sz = r2.Length();

            if (sx < 1e-6f || sy < 1e-6f || sz < 1e-6f)
            {
                outScale = Vec3::One();
                outRotation = Quat::Identity();
                return false;
            }

            outScale = Vec3(sx, sy, sz);

            r0 /= sx;
            r1 /= sy;
            r2 /= sz;

            outRotation = Quat::FromRotationMatrix3x3(
                r0.x, r0.y, r0.z,
                r1.x, r1.y, r1.z,
                r2.x, r2.y, r2.z
            );

            return true;
        }
    };

    inline Mat4 Quat::ToMat4() const
    {
        return Mat4::CreateFromQuaternion(*this);
    }
}
