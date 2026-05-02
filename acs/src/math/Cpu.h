// ACS Math — Runtime CPU feature detection (CPUID).
//
// One-time detection at first call; subsequent calls return cached values.
// Thread-safe via atomic init flag.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

struct CpuFeatures {
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse41;
    bool sse42;
    bool avx;
    bool avx2;
    bool fma3;
    bool f16c;
    bool bmi1;
    bool bmi2;
    bool aes;
    bool popcnt;
    bool avx512f;
};

const CpuFeatures& Cpu() noexcept;

// Convenience.
ACS_FORCEINLINE bool HasAvx2() noexcept { return Cpu().avx2; }
ACS_FORCEINLINE bool HasSse41() noexcept { return Cpu().sse41; }

} // namespace acs
