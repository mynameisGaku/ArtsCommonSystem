// ACS Foundation — Compiler & platform detection macros.
// Header-only. Zero dependencies.
#pragma once

// ---- Compiler detection -----------------------------------------------------
#if defined(__clang__)
    #define ACS_COMPILER_CLANG 1
    #define ACS_COMPILER_NAME "clang"
#elif defined(_MSC_VER)
    #define ACS_COMPILER_MSVC 1
    #define ACS_COMPILER_NAME "msvc"
#elif defined(__GNUC__)
    #define ACS_COMPILER_GCC 1
    #define ACS_COMPILER_NAME "gcc"
#else
    #error "ACS: unsupported compiler"
#endif

#ifndef ACS_COMPILER_CLANG
    #define ACS_COMPILER_CLANG 0
#endif
#ifndef ACS_COMPILER_MSVC
    #define ACS_COMPILER_MSVC 0
#endif
#ifndef ACS_COMPILER_GCC
    #define ACS_COMPILER_GCC 0
#endif

// ---- Platform detection -----------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
    #define ACS_PLATFORM_WINDOWS 1
    #define ACS_PLATFORM_NAME "windows"
#else
    #error "ACS Phase 1 currently targets Windows only"
#endif

// ---- Architecture -----------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
    #define ACS_ARCH_X64 1
    #define ACS_ARCH_NAME "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
    #define ACS_ARCH_ARM64 1
    #define ACS_ARCH_NAME "arm64"
#else
    #error "ACS: unsupported architecture"
#endif

#ifndef ACS_ARCH_X64
    #define ACS_ARCH_X64 0
#endif
#ifndef ACS_ARCH_ARM64
    #define ACS_ARCH_ARM64 0
#endif

// ---- Build mode -------------------------------------------------------------
#if defined(_DEBUG) || defined(DEBUG)
    #define ACS_BUILD_DEBUG 1
#else
    #define ACS_BUILD_DEBUG 0
#endif

// ---- Function attributes ----------------------------------------------------
#if ACS_COMPILER_MSVC
    #define ACS_FORCEINLINE     __forceinline
    #define ACS_NEVERINLINE     __declspec(noinline)
    #define ACS_RESTRICT        __restrict
    #define ACS_NORETURN        [[noreturn]]
    #define ACS_DEBUGBREAK()    __debugbreak()
    #define ACS_PRETTY_FUNC     __FUNCSIG__
    #define ACS_THREAD_LOCAL    __declspec(thread)
    #define ACS_CACHELINE_ALIGN __declspec(align(64))
    #define ACS_DLLEXPORT       __declspec(dllexport)
    #define ACS_DLLIMPORT       __declspec(dllimport)
#else
    #define ACS_FORCEINLINE     inline __attribute__((always_inline))
    #define ACS_NEVERINLINE     __attribute__((noinline))
    #define ACS_RESTRICT        __restrict__
    #define ACS_NORETURN        [[noreturn]]
    #define ACS_DEBUGBREAK()    __builtin_trap()
    #define ACS_PRETTY_FUNC     __PRETTY_FUNCTION__
    #define ACS_THREAD_LOCAL    __thread
    #define ACS_CACHELINE_ALIGN __attribute__((aligned(64)))
    #define ACS_DLLEXPORT       __attribute__((visibility("default")))
    #define ACS_DLLIMPORT
#endif

// ---- Branch hints -----------------------------------------------------------
#if ACS_COMPILER_MSVC
    // MSVC has no portable __builtin_expect; rely on PGO. Provide identity macro.
    #define ACS_LIKELY(x)   (x)
    #define ACS_UNLIKELY(x) (x)
#else
    #define ACS_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define ACS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// ---- Cache line size --------------------------------------------------------
// 64 on x64 / Apple silicon; 128 on some POWER chips. We target x64 / ARM64.
#define ACS_CACHELINE_SIZE 64

// ---- Misc -------------------------------------------------------------------
#define ACS_UNUSED(x) ((void)(x))

// Token paste helpers used by the assert / log macros.
#define ACS_CONCAT_INNER(a, b) a##b
#define ACS_CONCAT(a, b)       ACS_CONCAT_INNER(a, b)
#define ACS_STRINGIFY_INNER(x) #x
#define ACS_STRINGIFY(x)       ACS_STRINGIFY_INNER(x)
