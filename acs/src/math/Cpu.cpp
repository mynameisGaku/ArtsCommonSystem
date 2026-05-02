#include "math/Cpu.h"
#include "threading/Atomic.h"
#include <intrin.h>

namespace acs {

namespace {

CpuFeatures   g_features {};
Atomic<u32>   g_inited {0};

void DetectInternal() noexcept {
    int regs[4] {};
    __cpuid(regs, 0);
    int max_leaf = regs[0];
    if (max_leaf < 1) return;

    __cpuid(regs, 1);
    int ecx1 = regs[2];
    int edx1 = regs[3];

    g_features.sse2   = (edx1 & (1 << 26)) != 0;
    g_features.sse3   = (ecx1 & (1 <<  0)) != 0;
    g_features.ssse3  = (ecx1 & (1 <<  9)) != 0;
    g_features.sse41  = (ecx1 & (1 << 19)) != 0;
    g_features.sse42  = (ecx1 & (1 << 20)) != 0;
    g_features.fma3   = (ecx1 & (1 << 12)) != 0;
    g_features.aes    = (ecx1 & (1 << 25)) != 0;
    g_features.f16c   = (ecx1 & (1 << 29)) != 0;
    g_features.popcnt = (ecx1 & (1 << 23)) != 0;

    bool osxsave = (ecx1 & (1 << 27)) != 0;
    bool avx_cpu = (ecx1 & (1 << 28)) != 0;
    bool avx_ok = false;
    if (osxsave && avx_cpu) {
        unsigned long long xcr = _xgetbv(0);
        avx_ok = (xcr & 0x6) == 0x6;
    }
    g_features.avx = avx_ok;

    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        int ebx7 = regs[1];
        g_features.avx2    = avx_ok && ((ebx7 & (1 <<  5)) != 0);
        g_features.bmi1    = (ebx7 & (1 <<  3)) != 0;
        g_features.bmi2    = (ebx7 & (1 <<  8)) != 0;
        g_features.avx512f = (ebx7 & (1 << 16)) != 0;
    }
}

} // namespace

const CpuFeatures& Cpu() noexcept {
    if (g_inited.Load(MemoryOrder::Acquire) == 0) {
        u32 expected = 0;
        if (g_inited.CompareExchange(expected, 1)) {
            DetectInternal();
            g_inited.Store(2, MemoryOrder::Release);
        } else {
            // Another thread is initializing — spin.
            while (g_inited.Load(MemoryOrder::Acquire) != 2) SpinHint();
        }
    }
    return g_features;
}

} // namespace acs
