// SPDX-License-Identifier: MIT
#pragma once

namespace acs::reference_fixture
{
template<typename T>
inline constexpr bool IsReferenceValueV = true;

inline int InlineValue(int value) noexcept
{
    return value;
}
}
