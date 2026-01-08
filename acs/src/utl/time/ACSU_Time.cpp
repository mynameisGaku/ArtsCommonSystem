#include "Time.h"
#include "ACSU_Time.h"

std::chrono::steady_clock::time_point ACSU_Time::s_prevTime;
float ACSU_Time::s_deltaTime = 0.0f;
float ACSU_Time::s_timeScale = 1.0f;
float ACSU_Time::s_fixedDeltaTime = 1.0f / 60.0f;
float ACSU_Time::s_fixedAccumulator = 0.0f;

void ACSU_Time::Init()
{
    s_prevTime = std::chrono::steady_clock::now();
    s_deltaTime = 0.0f;
    s_fixedAccumulator = 0.0f;
}

void ACSU_Time::Update()
{
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<float> diff = now - s_prevTime;
    s_prevTime = now;

    s_deltaTime = diff.count();
    s_fixedAccumulator += s_deltaTime;
}

float ACSU_Time::DeltaTime()
{
    return s_deltaTime * s_timeScale;
}

float ACSU_Time::FixedDeltaTime()
{
    return s_fixedDeltaTime;
}

float ACSU_Time::GetTimeScale()
{
    return s_timeScale;
}

void ACSU_Time::SetTimeScale(float scale)
{
    s_timeScale = scale;
}

bool ACSU_Time::ConsumeFixedStep()
{
    if (s_fixedAccumulator >= s_fixedDeltaTime)
    {
        s_fixedAccumulator -= s_fixedDeltaTime;
        return true;
    }
    return false;
}
