// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — CPU 機能検出（CPUID 実行時判定）
// -----------------------------------------------------------------------------
// SSE / AVX / FMA / BMI などの命令セット対応有無をスレッドセーフに 1 度だけ
// 検出し、結果をキャッシュする。MathDispatch が起動時に問い合わせて
// 関数ポインタテーブルを構築するのに使う。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

// 検出された CPU 機能フラグ
struct CpuFeatures {
    bool sse2;     // SSE2 (x64 ベースライン、常に true)
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

// CPU 機能を取得（初回呼び出しで CPUID を実行、以降キャッシュ）
const CpuFeatures& Cpu() noexcept;

// 簡易判定 (頻出のもの)
ACS_FORCEINLINE bool HasAvx2()  noexcept { return Cpu().avx2; }
ACS_FORCEINLINE bool HasSse41() noexcept { return Cpu().sse41; }

} // namespace acs
