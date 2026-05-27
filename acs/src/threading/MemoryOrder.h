// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — メモリ順序タグとバリア
// -----------------------------------------------------------------------------
// std::memory_order の代替。呼び出し側のコードを std と同じ語彙で書けるよう
// にしつつ、実装は MSVC の _Interlocked* + サフィックス付き組み込み
// (m_Acq / m_Rel / m_Nf) を直接使う。
//
// 注意: x64 では各 _Interlocked* は完全バリア相当。EMemoryOrder の指定は
// 主にコンパイラ最適化への抑制ヒントとして機能する。ARM64 ではサフィックス
// 付きを使い分けて余計な dmb 命令を省く。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"
#include <intrin.h>

namespace acs {

// ---- メモリ順序の種類 ----------------------------------------------------
// std::memory_order の対訳:
//   Relaxed = relaxed (順序保証なし)
//   Acquire = acquire (後続のロード/ストアを上に動かさない)
//   Release = release (先行のロード/ストアを下に動かさない)
//   AcqRel  = acq_rel (RMW で両方)
//   SeqCst  = seq_cst (全スレッドで全順序)
enum class EMemoryOrder : int {
    Relaxed = 0,
    Acquire = 1,
    Release = 2,
    AcqRel  = 3,
    SeqCst  = 4,
};

// ---- コンパイラのみのバリア ----------------------------------------------
// CPU 命令は出さない。コンパイラに対して「ここを跨いで命令を並び替えるな」
// という指示のみ。MSVC は _ReadWriteBarrier 廃止予定だが現状動作する。
ACS_FORCEINLINE void CompilerBarrier() noexcept {
#if ACS_COMPILER_MSVC
    _ReadWriteBarrier();
#else
    asm volatile("" ::: "memory");
#endif
}

// ---- ハードウェアフェンス（CPU + コンパイラ両方）------------------------
// 全 RMW 操作を確実に順序付けたい場合に使う。x64 では mfence、ARM64 では dmb。
ACS_FORCEINLINE void HardwareFence() noexcept {
#if ACS_ARCH_X64
    _mm_mfence();
#elif ACS_ARCH_ARM64
    __dmb(0xB); // ISH (Inner Shareable)
#endif
}

// ---- スピン待ちでのヒント命令 --------------------------------------------
// busy-wait ループ内で 1 回入れることで、ハイパースレッディングのパートナー
// に CPU リソースを譲り、消費電力と発熱を抑える。
ACS_FORCEINLINE void SpinHint() noexcept {
#if ACS_ARCH_X64
    _mm_pause();      // x86 PAUSE 命令
#elif ACS_ARCH_ARM64
    __yield();        // ARMv8 YIELD 命令
#endif
}

} // namespace acs
