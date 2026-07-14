// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — コンパイラ／プラットフォーム検出マクロ
// -----------------------------------------------------------------------------
// MSVC / Clang / GCC、Windows / x64 / ARM64、Debug / Release などの判定マクロ、
// および ACS_FORCEINLINE / ACS_NORETURN / ACS_LIKELY 等の関数属性ラッパを提供。
//
// 本ヘッダはどこからでも include 可能であり、依存を持たない。
// =============================================================================
#pragma once

// コンパイラ固有のマクロ群（_MSC_VER, __clang__, __GNUC__）を見て
// ACS_COMPILER_MSVC / ACS_COMPILER_CLANG / ACS_COMPILER_GCC のいずれかを 1 にする。
#if defined(__clang__)
#    define ACS_COMPILER_CLANG 1
#    define ACS_COMPILER_NAME  "clang"
#elif defined(_MSC_VER)
#    define ACS_COMPILER_MSVC 1
#    define ACS_COMPILER_NAME "msvc"
#elif defined(__GNUC__)
#    define ACS_COMPILER_GCC  1
#    define ACS_COMPILER_NAME "gcc"
#else
#    error "ACS: 未対応のコンパイラ"
#endif

// 未定義のフラグは 0 で埋めて、`#if ACS_COMPILER_*` を常に成立させる。
#ifndef ACS_COMPILER_CLANG
#    define ACS_COMPILER_CLANG 0
#endif
#ifndef ACS_COMPILER_MSVC
#    define ACS_COMPILER_MSVC 0
#endif
#ifndef ACS_COMPILER_GCC
#    define ACS_COMPILER_GCC 0
#endif

// ACS は Windows / DX12 をターゲットとする。
#if defined(_WIN32) || defined(_WIN64)
#    define ACS_PLATFORM_WINDOWS 1
#    define ACS_PLATFORM_NAME    "windows"
#else
#    error "ACS currently targets Windows only"
#endif

// x86-64 と ARM64 をサポート。SIMD やスレッド原語の選択に使用。
#if defined(_M_X64) || defined(__x86_64__)
#    define ACS_ARCH_X64  1
#    define ACS_ARCH_NAME "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#    define ACS_ARCH_ARM64 1
#    define ACS_ARCH_NAME  "arm64"
#else
#    error "ACS: 未対応のアーキテクチャ"
#endif

#ifndef ACS_ARCH_X64
#    define ACS_ARCH_X64 0
#endif
#ifndef ACS_ARCH_ARM64
#    define ACS_ARCH_ARM64 0
#endif

// Debug / Release を統一的に判定するためのフラグ。
#if defined(_DEBUG) || defined(DEBUG)
#    define ACS_BUILD_DEBUG 1
#else
#    define ACS_BUILD_DEBUG 0
#endif

// ASan 有効時はコンパイラとランタイムが通常のヒープ API を差し替える。
// CRT デバッグヒープなど、別の割り当て追跡機構との排他判定にも使用する。
#if defined(__SANITIZE_ADDRESS__)
#    define ACS_ADDRESS_SANITIZER 1
#else
#    define ACS_ADDRESS_SANITIZER 0
#endif

// コンパイラ間の方言差を吸収し、ポータブルな属性マクロを提供する。
#if ACS_COMPILER_MSVC
#    define ACS_FORCEINLINE     __forceinline         // 強制インライン化（MSVC）
#    define ACS_NEVERINLINE     __declspec(noinline)  // インライン抑制
#    define ACS_RESTRICT        __restrict            // restrict 修飾子
#    define ACS_NORETURN        [[noreturn]]          // 戻らない関数
#    define ACS_DEBUGBREAK()    __debugbreak()        // デバッガでブレーク
#    define ACS_PRETTY_FUNC     __FUNCSIG__           // 関数シグネチャ文字列
#    define ACS_THREAD_LOCAL    __declspec(thread)    // スレッドローカル変数
#    define ACS_CACHELINE_ALIGN __declspec(align(64)) // キャッシュライン整列（64B）
#    define ACS_DLLEXPORT       __declspec(dllexport) // DLL シンボルエクスポート
#    define ACS_DLLIMPORT       __declspec(dllimport) // DLL シンボルインポート
#else
#    define ACS_FORCEINLINE     inline __attribute__((always_inline))
#    define ACS_NEVERINLINE     __attribute__((noinline))
#    define ACS_RESTRICT        __restrict__
#    define ACS_NORETURN        [[noreturn]]
#    define ACS_DEBUGBREAK()    __builtin_trap()
#    define ACS_PRETTY_FUNC     __PRETTY_FUNCTION__
#    define ACS_THREAD_LOCAL    __thread
#    define ACS_CACHELINE_ALIGN __attribute__((aligned(64)))
#    define ACS_DLLEXPORT       __attribute__((visibility("default")))
#    define ACS_DLLIMPORT
#endif

// 特定の関数だけ AVX / AVX2 を有効化するための属性。
// Clang / GCC は組み込み関数 (_mm256_* 等) をターゲット機能でゲートするため、
// AVX 命令を使う関数には per-function の target 属性が必要。MSVC は組み込み
// 関数をゲートしないので空でよい。
// 用途: ランタイム CPU 判定で AVX 経路を選ぶコードで、AVX 組み込み関数を使う
//       関数に付与する（ベースラインは SSE2 のまま、その関数だけ AVX 許可）。
#if ACS_COMPILER_MSVC
#    define ACS_TARGET_AVX
#    define ACS_TARGET_AVX2
#else
#    define ACS_TARGET_AVX  __attribute__((target("avx")))
#    define ACS_TARGET_AVX2 __attribute__((target("avx2,fma")))
#endif

// CPU の分岐予測器に「成立／不成立」のヒントを与え、ホットパスを最適化する。
// MSVC は __builtin_expect 相当を持たないため、PGO に依存して恒等マクロ化。
#if ACS_COMPILER_MSVC
#    define ACS_LIKELY(x)   (x)
#    define ACS_UNLIKELY(x) (x)
#else
#    define ACS_LIKELY(x)   __builtin_expect(!!(x), 1)
#    define ACS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// x64 / Apple Silicon では 64 バイト。POWER 系は 128 だが現状非対応。
#define ACS_CACHELINE_SIZE 64

#define ACS_UNUSED(x)      ((void)(x)) // 未使用警告抑制

// トークン連結／文字列化マクロ（Assert 等のマクロで使用）
#define ACS_CONCAT_INNER(a, b) a##b
#define ACS_CONCAT(a, b)       ACS_CONCAT_INNER(a, b)
#define ACS_STRINGIFY_INNER(x) #x
#define ACS_STRINGIFY(x)       ACS_STRINGIFY_INNER(x)
