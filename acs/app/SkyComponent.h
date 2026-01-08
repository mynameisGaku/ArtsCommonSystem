#pragma once

#include <Pch.h>

class SkyComponent : public Component
{
public:
    struct SetupParam
    {
        float Radius = 5000.0f;
        int Slices = 64;
        int Stacks = 32;

        bool DisableLighting = true;
        bool DisableZBuffer = true;

        // コンパイル済みPSバイナリ(.pso)へのパス
        const char* PixelShaderPath = nullptr;
    };

    struct SkyParamsCB
    {
        float SunDirX;
        float SunDirY;
        float SunDirZ;
        float Intensity;

        float Turbidity;
        float Exposure;
        float Time;
        float Padding;
    };


public:
    bool Setup(const SetupParam& p);

    void Start() override;
    void Update(float dt) override;
    void Draw(float dt) override;
    void Destroy() override;

private:
    void EnsureCreated();
    void RebuildMesh();
    void UpdateParamTextureIfNeeded();

    static unsigned char ToUNormByte(float v01);
    static unsigned char ToSNormByte(float vNeg1To1);

private:
    float m_Time = 0.0f;

    SetupParam m_Param;

    int m_PS = -1;

    int m_ParamSoftImage = -1; // 4x1
    int m_ParamGraph = -1;

    int m_PSCBuffer = -1;

    std::vector<VERTEX3DSHADER> m_UnitVertices;
    std::vector<VERTEX3DSHADER> m_DrawVertices;
    std::vector<unsigned short> m_Indices;

    bool m_Created = false;

    std::uint32_t m_LastSunVer = 0;
    std::uint32_t m_LastIntVer = 0;
    std::uint32_t m_LastTurbVer = 0;
    std::uint32_t m_LastExpoVer = 0;
};
