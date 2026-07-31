// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * FRandom の再生位置と破損検査値を保持する値型。
 *
 * @details 各fieldを明示的に符号化して保存し、この型の生byte列は永続化しない。
 */
struct FRandomSnapshot {
    /** 保存形式の版。 */
    u32 version = 0u;

    /** xoshiro128** の第0状態。 */
    u32 state0 = 0u;

    /** xoshiro128** の第1状態。 */
    u32 state1 = 0u;

    /** xoshiro128** の第2状態。 */
    u32 state2 = 0u;

    /** xoshiro128** の第3状態。 */
    u32 state3 = 0u;

    /** 将来拡張用の予約値。現行版では0だけを受け付ける。 */
    u32 reserved = 0u;

    /** 版、状態、予約値をlittle-endian順に検査した値。 */
    u64 signature = 0u;

    /** 現行の保存形式版。 */
    static constexpr u32 kCurrentVersion = 1u;
};

} // namespace acs::game
