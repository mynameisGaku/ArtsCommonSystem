#pragma once
#include <cstdint>

namespace BlackboardKeys
{
    static constexpr std::uint32_t TransformWorldTRS = 0x1001;

    static constexpr std::uint32_t SunDirection = 0x3101; // ACSU_Math::Vector3
    static constexpr std::uint32_t SunIntensity = 0x3102; // float
    static constexpr std::uint32_t Turbidity = 0x3103;    // float (0..1‘z’è)
    static constexpr std::uint32_t Exposure = 0x3104;     // float (0..1‘z’è)
}
