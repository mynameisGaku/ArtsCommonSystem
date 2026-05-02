// ACS Threading — Memory order tags & barriers (no <atomic>).
//
// We mirror the C++ memory-order vocabulary so callers stay portable, but the
// underlying implementation uses the suffixed Win32 _Interlocked* intrinsics
// (_acq, _rel, _nf) on ARM64 and the unsuffixed full-barrier variants on x64
// (where every interlocked op is a full fence at the hardware level anyway).
#pragma once

#include "foundation/Compiler.h"
#include <intrin.h>

namespace acs {

enum class MemoryOrder : int {
    Relaxed = 0,    // no ordering
    Acquire = 1,    // load: subsequent loads/stores cannot move above
    Release = 2,    // store: previous loads/stores cannot move below
    AcqRel  = 3,    // RMW: both
    SeqCst  = 4,    // total order
};

// Compiler-only fence (prevents reordering across the call but emits no asm).
ACS_FORCEINLINE void CompilerBarrier() noexcept {
#if ACS_COMPILER_MSVC
    _ReadWriteBarrier();
#else
    asm volatile("" ::: "memory");
#endif
}

// Full HW + compiler fence.
ACS_FORCEINLINE void HardwareFence() noexcept {
#if ACS_ARCH_X64
    _mm_mfence();
#elif ACS_ARCH_ARM64
    __dmb(0xB); // ISH
#endif
}

// Pause / yield hint inside spin loops.
ACS_FORCEINLINE void SpinHint() noexcept {
#if ACS_ARCH_X64
    _mm_pause();
#elif ACS_ARCH_ARM64
    __yield();
#endif
}

} // namespace acs
