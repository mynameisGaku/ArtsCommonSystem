#pragma once

#include <Pch.h>
#include "SkyTextureGenerator.h"

class SkyMeshComponent : public Component
{
public:
    struct SetupParam
    {
        int TexW = 256;
        int TexH = 128;

        float Radius = 5000.0f;

        int Slices = 64;
        int Stacks = 32;

        bool DisableLighting = true;
        bool DisableZBuffer = true;
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
    void UpdateSkyTexture();

private:
    SetupParam m_Param;

    int m_SkySoftImage = -1;
    int m_SkyTexGraph = -1;

    SkyTextureGenerator* m_Gen = nullptr;
    SkyTextureGenerator::Params m_GenParams;

    std::vector<VERTEX3D> m_UnitVertices;
    std::vector<VERTEX3D> m_DrawVertices;
    std::vector<unsigned short> m_Indices;

    bool m_Created = false;
    std::uint32_t m_LastSunVersion = 0;
};
