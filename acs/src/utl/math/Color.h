#pragma once
#include "Mathf.h"
#include "Vector4.h"

namespace ACSU_Math
{
    struct Color
    {
        float r, g, b, a;
        Color() : r(0), g(0), b(0), a(1) {}
        Color(float _r, float _g, float _b, float _a = 1.0f) : r(_r), g(_g), b(_b), a(_a) {}

        Color(const Vector4& v) : r(v.x), g(v.y), b(v.z), a(v.w) {}
        operator Vector4() const { return Vector4(r, g, b, a); }
        Vector4 ToVector4() const { return Vector4(r, g, b, a); }

        static Color HSVToRGB(float H, float S, float V, bool hdr = false)
        {
            if (S == 0.0f) return Color(V, V, V, 1.0f);

            H = H - Mathf::Floor(H);
            float var_h = H * 6.0f;
            if (var_h == 6.0f) var_h = 0.0f;

            int var_i = (int)var_h;
            float var_1 = V * (1.0f - S);
            float var_2 = V * (1.0f - S * (var_h - var_i));
            float var_3 = V * (1.0f - S * (1.0f - (var_h - var_i)));

            float r = V, g = V, b = V;

            switch (var_i)
            {
            case 0: r = V;     g = var_3; b = var_1; break;
            case 1: r = var_2; g = V;     b = var_1; break;
            case 2: r = var_1; g = V;     b = var_3; break;
            case 3: r = var_1; g = var_2; b = V;     break;
            case 4: r = var_3; g = var_1; b = V;     break;
            default: r = V;    g = var_1; b = var_2; break;
            }
            return Color(r, g, b, 1.0f);
        }

        friend inline Color operator+(const Color& a, const Color& b) { return Color(a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a); }
        friend inline Color operator-(const Color& a, const Color& b) { return Color(a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a); }
        friend inline Color operator*(const Color& a, float b) { return Color(a.r * b, a.g * b, a.b * b, a.a * b); }
        friend inline Color operator*(float b, const Color& a) { return Color(a.r * b, a.g * b, a.b * b, a.a * b); }
    };
}