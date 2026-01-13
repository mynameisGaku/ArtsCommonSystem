#pragma once

#include <Pch.h>

class GOC_Sprite : public GO_Component
{
public:
    struct SetupParam
    {
        const char* Path = nullptr;
        int DivX = 1;
        int DivY = 1;
        int CellW = 0;
        int CellH = 0;
        bool Loop = true;
        float FrameTime = 0.1f;
    };

    void Setup(const SetupParam& p);

    void Update(float dt) override;
    void Draw(float dt) override;
    void Destroy() override;

private:
    void EnsureLoaded();

private:
    SetupParam m_Param;

	std::vector<int> m_Handles;
    int m_Frame = 0;
    float m_Timer = 0.0f;
};
