// SPDX-License-Identifier: Apache-2.0
// ACS Threading — メモリ順序タグとバリア
//
// std::memory_order の代替。呼び出し側のコードを std と同じ語彙で書けるよう
// にしつつ、実装は MSVC の _Interlocked* + サフィックス付き組み込み
// (_acq / _rel / _nf) を直接使う。
//
// 注意: x64 では各 _Interlocked* は完全バリア相当。EMemoryOrder の指定は
// 主にコンパイラ最適化への抑制ヒントとして機能する。ARM64 ではサフィックス
// 付きを使い分けて余計な dmb 命令を省く。
#pragma once

#include "foundation/Compiler.h"
#include <intrin.h>

namespace acs {

/**
 * アトミック操作のメモリ順序タグ (std::memory_order の対訳)。
 *
 * @details
 * Relaxed=順序保証なし、Acquire=後続のロード/ストアを上へ動かさない、
 * Release=先行のロード/ストアを下へ動かさない、AcqRel=RMW で両方、
 * SeqCst=全スレッドで全順序。
 */
enum class EMemoryOrder : int {
    /** 順序保証なし (relaxed)。アトミック性のみを保証する最高速モード。 */
    Relaxed = 0,

    /** acquire。後続のロード/ストアをこの操作より前へ動かさない。 */
    Acquire = 1,

    /** release。先行のロード/ストアをこの操作より後へ動かさない。 */
    Release = 2,

    /** acq_rel。RMW 操作で acquire と release の両方を満たす。 */
    AcqRel  = 3,

    /** seq_cst。全スレッドで観測される単一の全順序を保証する。 */
    SeqCst  = 4,
};

/**
 * コンパイラのみのバリアを張る (CPU 命令は出さない)。
 *
 * @details
 * コンパイラに対して「ここを跨いで命令を並び替えるな」という指示のみで、
 * ハードウェアのメモリ順序には影響しない。MSVC は _ReadWriteBarrier が
 * 廃止予定だが現状動作する。
 */
ACS_FORCEINLINE void CompilerBarrier() noexcept {
#if ACS_COMPILER_MSVC
    _ReadWriteBarrier();
#else
    asm volatile("" ::: "memory");
#endif
}

/**
 * ハードウェアフェンス (CPU + コンパイラ両方) を張る。
 *
 * @details
 * 全 RMW 操作を確実に順序付けたい場合に使う。x64 では mfence、ARM64 では
 * dmb ish (Inner Shareable) を発行する。
 */
ACS_FORCEINLINE void HardwareFence() noexcept {
#if ACS_ARCH_X64
    _mm_mfence();
#elif ACS_ARCH_ARM64
    __dmb(0xB); // ISH (Inner Shareable)
#endif
}

/**
 * スピン待ちで CPU にヒント命令を 1 回発行する。
 *
 * @details
 * busy-wait ループ内で入れることで、ハイパースレッディングのパートナーに
 * CPU リソースを譲り、消費電力と発熱を抑える。x64 では PAUSE、ARM64 では
 * YIELD を発行する。
 */
ACS_FORCEINLINE void SpinHint() noexcept {
#if ACS_ARCH_X64
    _mm_pause();      // x86 PAUSE 命令
#elif ACS_ARCH_ARM64
    __yield();        // ARMv8 YIELD 命令
#endif
}

} // namespace acs
