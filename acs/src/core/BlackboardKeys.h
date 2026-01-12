#pragma once
#include <cstdint>

namespace BlackboardKeys
{
    static constexpr std::uint32_t TransformWorldTRS = 0x1001;

    static constexpr std::uint32_t SunDirection = 0x3101; // ACSU_Math::Vector3

    // --- Unity Skybox Parameters ---
    static constexpr std::uint32_t SkyTint = 0x3102; // Vector3 (Color)
    static constexpr std::uint32_t GroundColor = 0x3103; // Vector3 (Color)
    static constexpr std::uint32_t HorizonColor = 0x3104; // Vector3 (Color) - ŒvZ‚Åo‚µ‚Ä‚à‚¢‚¢‚ªA§Œä‚Å‚«‚½•û‚ªŠy

    static constexpr std::uint32_t SunSize = 0x3105; // float
    static constexpr std::uint32_t SunConvergence = 0x3106; // float
    static constexpr std::uint32_t AtmosphereThick = 0x3107; // float
    static constexpr std::uint32_t Exposure = 0x3108; // float
	static constexpr std::uint32_t Turbidity = 0x3109; // float - ‰_‚Ì”Z‚³
}