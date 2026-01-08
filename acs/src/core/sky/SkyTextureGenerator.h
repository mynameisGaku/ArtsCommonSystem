#pragma once

#include <Pch.h>

class SkyTextureGenerator
{
public:
    struct Params
    {
        float Turbidity = 2.0f;   // 霞っぽさ
        float Horizon = 0.02f;    // 地平線の白化強さ
        float Exposure = 1.2f;    // 露出
        float SunDisk = 0.996f;   // 太陽円盤閾値（大きいほど小さい）
        float SunGlow = 0.25f;    // 太陽周辺のにじみ
    };

public:
    SkyTextureGenerator(int w, int h)
        : m_W(w), m_H(h)
    {
        m_Pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    }

    int Width() const { return m_W; }
    int Height() const { return m_H; }

    const std::uint8_t* PixelsRGBA() const
    {
        return m_Pixels.data();
    }

    void Render(const ACSU_Math::Vec3& sunDirWorld, float sunIntensity, const Params& p)
    {
        ACSU_Math::Vec3 sunDir = NormalizeSafe(sunDirWorld);

        for (int y = 0; y < m_H; ++y)
        {
            for (int x = 0; x < m_W; ++x)
            {
                // equirectangular: u=[0,1), v=[0,1]
                float u = (x + 0.5f) / static_cast<float>(m_W);
                float v = (y + 0.5f) / static_cast<float>(m_H);

                // 方向ベクトル（Y+が上）
                float phi = (u * 2.0f - 1.0f) * 3.1415926535f;     // -pi..pi
                float theta = v * 3.1415926535f;                   // 0..pi

                ACSU_Math::Vec3 dir;
                dir.x = std::cos(phi) * std::sin(theta);
                dir.y = std::cos(theta);
                dir.z = std::sin(phi) * std::sin(theta);
                dir = NormalizeSafe(dir);

                float mu = Clamp01(Dot(dir, sunDir));              // 太陽との角度
                float up01 = Clamp01(dir.y * 0.5f + 0.5f);          // 上ほど1

                // 地平線付近を白っぽく
                float h = SmoothStep(p.Horizon, 1.0f, up01);

                // ベース色（天頂→青、地平線→薄青白）
                float3 zenith = { 0.20f, 0.35f, 0.85f };
                float3 horizon = { 0.78f, 0.82f, 0.92f };

                float day = Clamp01(sunDir.y * 0.5f + 0.5f);   // 0..1
                float night = 1.0f - day;

                float3 nightZenith = { 0.01f, 0.02f, 0.05f };
                float3 nightHorizon = { 0.02f, 0.02f, 0.03f };

                float3 baseDay = Lerp(horizon, zenith, h);
                float3 baseNight = Lerp(nightHorizon, nightZenith, h);

                float3 base = Lerp(baseNight, baseDay, day);

                // “散乱っぽい”項（軽量）
                float rayleigh = std::pow(mu, 4.0f);
                float mie = std::pow(mu, 1.0f / std::max(p.Turbidity, 0.01f));

                // 太陽高度で夕焼け寄り（太陽が低いほど赤み）
                float sunHeight = Clamp01(sunDir.y * 0.5f + 0.5f);
                float sunset = Clamp01(1.0f - sunHeight);
                float3 sunCol = { 1.0f, 0.98f, 0.92f };
                sunCol = Lerp(sunCol, float3{ 1.0f, 0.55f, 0.20f }, sunset * 0.8f);

                float3 col = base;
                col = Add(col, Mul(sunCol, sunIntensity * (0.12f * rayleigh + 0.30f * mie)));

                // 太陽円盤 + グロー
                float disk = SmoothStep(p.SunDisk, 1.0f, mu);
                float glow = std::pow(mu, 12.0f) * p.SunGlow;

                col = Add(col, Mul(sunCol, sunIntensity * (disk * 1.2f + glow)));

                // 露出トーンマップ
                col.x = 1.0f - std::exp(-col.x * p.Exposure);
                col.y = 1.0f - std::exp(-col.y * p.Exposure);
                col.z = 1.0f - std::exp(-col.z * p.Exposure);

                WritePixel(x, y, col);
            }
        }
    }

private:
    struct float3 { float x, y, z; };

    static float Clamp01(float v)
    {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    static float SmoothStep(float a, float b, float x)
    {
        float t = Clamp01((x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    static float Dot(const ACSU_Math::Vec3& a, const ACSU_Math::Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static ACSU_Math::Vec3 NormalizeSafe(const ACSU_Math::Vec3& v)
    {
        float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len < 1e-6f)
        {
            return ACSU_Math::Vec3(0.0f, 1.0f, 0.0f);
        }
        return ACSU_Math::Vec3(v.x / len, v.y / len, v.z / len);
    }

    static float3 Lerp(const float3& a, const float3& b, float t)
    {
        float3 r;
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        return r;
    }

    static float3 Add(const float3& a, const float3& b)
    {
        return float3{ a.x + b.x, a.y + b.y, a.z + b.z };
    }

    static float3 Mul(const float3& a, float s)
    {
        return float3{ a.x * s, a.y * s, a.z * s };
    }

    void WritePixel(int x, int y, const float3& c)
    {
        size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(m_W) + static_cast<size_t>(x)) * 4u;

        m_Pixels[idx + 0] = ToByte(c.x);
        m_Pixels[idx + 1] = ToByte(c.y);
        m_Pixels[idx + 2] = ToByte(c.z);
        m_Pixels[idx + 3] = 255;
    }

    static std::uint8_t ToByte(float v)
    {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    }

private:
    int m_W = 0;
    int m_H = 0;
    std::vector<std::uint8_t> m_Pixels;
};
