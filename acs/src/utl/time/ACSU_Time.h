#pragma once
#include <Pch.h>

class ACSU_Time
{
public:
    static void Init();
    static void Update();

    static float DeltaTime();
    static float FixedDeltaTime();

	static float GetTimeScale();
	static void SetTimeScale(float scale);

    static bool ConsumeFixedStep();

private:
    static std::chrono::steady_clock::time_point s_prevTime;
    static float s_deltaTime;
    static float s_fixedDeltaTime;
    static float s_fixedAccumulator;
    static float s_timeScale;
};
