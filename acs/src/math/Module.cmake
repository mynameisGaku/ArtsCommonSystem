acs_module(
    NAME    Math
    TYPE    Runtime
    SOURCES
        Cpu.cpp
        MathDispatch.cpp
    HEADERS
        Cpu.h
        Math.h
        Vec.h
        Mat.h
        Quat.h
        Camera.h
        Collision2D.h
        Collision3D.h
        MathDispatch.h
    PUBLIC_DEPS
        Foundation
        Threading
)

acs_module_feature(MODULE Math NAME DIRECTXMATH
    DEFINE MATH_DIRECTXMATH
    DESCRIPTION "Use DirectXMath as the SIMD backend (recommended on Windows)"
    DEFAULT ON)

acs_module_feature(MODULE Math NAME AVX2
    DEFINE MATH_AVX2
    DESCRIPTION "Compile AVX2 fast paths for batch math operations"
    DEFAULT ON)

acs_module_feature(MODULE Math NAME RUNTIME_DISPATCH
    DEFINE MATH_RUNTIME_DISPATCH
    DESCRIPTION "Use runtime CPU detection to select SIMD path (vs static)"
    DEFAULT ON)
