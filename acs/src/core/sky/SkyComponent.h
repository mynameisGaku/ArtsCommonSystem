#pragma once

#include <Pch.h>
#include "SkyTextureGenerator.h"

class SkyComponent : public Component
{
public:
    struct SetupParam
    {
        const char* SkyDomeModelPath = nullptr;
        int TexW = 256;
        int TexH = 128;
        float DomeScale = 5000.0f;
    };

    bool Setup(const SetupParam& p);

    void Start() override;
    void Update(float dt) override;
    void Draw(float dt) override;
    void Destroy() override;

private:
    void EnsureLoaded();
    void UpdateSkyTexture();

private:
    SetupParam m_Param;

    int m_Model = -1;
    int m_SkyTexGraph = -1;
    int m_SkySoftImage = -1;
    int m_TexIndex = -1;

    SkyTextureGenerator* m_Gen = nullptr;
    SkyTextureGenerator::Params m_GenParams;

    bool m_Loaded = false;
    std::uint32_t m_LastSunVersion = 0;
};
