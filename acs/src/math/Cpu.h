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

/**
 * CPUID で検出した CPU 命令セット対応フラグの集合。
 *
 * @details 各フィールドは対応していれば true。MathDispatch が経路選択に使う。
 */
struct CpuFeatures {
    /** SSE2 対応 (x64 ベースラインのため常に true)。 */
    bool sse2;

    /** SSE3 対応。 */
    bool sse3;

    /** SSSE3 対応。 */
    bool ssse3;

    /** SSE4.1 対応。 */
    bool sse41;

    /** SSE4.2 対応。 */
    bool sse42;

    /** AVX 対応 (OS 側 XSAVE 有効化も確認済み)。 */
    bool avx;

    /** AVX2 対応 (AVX が OS 有効な場合のみ true)。 */
    bool avx2;

    /** FMA3 (積和演算) 対応。 */
    bool fma3;

    /** F16C (half float 変換) 対応。 */
    bool f16c;

    /** BMI1 (ビット操作命令) 対応。 */
    bool bmi1;

    /** BMI2 (ビット操作命令) 対応。 */
    bool bmi2;

    /** AES-NI (暗号化命令) 対応。 */
    bool aes;

    /** POPCNT (ビットカウント) 対応。 */
    bool popcnt;

    /** AVX-512 Foundation 対応。 */
    bool avx512f;
};

/**
 * 検出済み CPU 機能を返す (初回呼び出しで CPUID 実行、以降キャッシュ)。
 *
 * @details 検出はスレッドセーフに 1 度だけ実行される。
 * @return 検出結果への const 参照。
 */
const CpuFeatures& Cpu() noexcept;

/**
 * AVX2 に対応しているかを返す。
 *
 * @return AVX2 対応なら true。
 */
ACS_FORCEINLINE bool HasAvx2()  noexcept { return Cpu().avx2; }

/**
 * SSE4.1 に対応しているかを返す。
 *
 * @return SSE4.1 対応なら true。
 */
ACS_FORCEINLINE bool HasSse41() noexcept { return Cpu().sse41; }

} // namespace acs
