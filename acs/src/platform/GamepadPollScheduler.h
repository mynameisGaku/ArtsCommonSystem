// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::detail {

/**
 * 接続中ポートを毎フレーム、未接続確認を順番に一つずつ選ぶ。
 *
 * @tparam PortCount 管理するゲームパッドポート数。1以上32以下。
 */
template <usize PortCount>
class TGamepadPollScheduler {
    static_assert(PortCount > 0, "ゲームパッド・スケジューラには1ポート以上が必要です");
    static_assert(PortCount <= 32, "ポーリングマスクが保持できるのは32ポートまでです");

public:
    /**
     * 今フレームに取得するポートをビットマスクで返す。
     *
     * @param connected 各ポートの直前フレームの接続状態。
     * @return 接続中の全ポートと、未接続確認用の最大1ポートを含むマスク。
     */
    constexpr u32 BuildPollMask(const bool (&connected)[PortCount]) noexcept
    {
        /** 今フレームに取得するポートのビット集合。 */
        u32 mask = 0;
        /** 接続中ポートを走査する添字。 */
        for (usize port_index = 0; port_index < PortCount; ++port_index) {
            if (connected[port_index]) mask |= u32{1} << static_cast<u32>(port_index);
        }

        /** 次の未接続ポートを探す相対位置。 */
        for (usize offset = 0; offset < PortCount; ++offset) {
            /** 今回接続を確認する候補ポート。 */
            const usize port_index = (m_NextDisconnected + offset) % PortCount;
            if (!connected[port_index]) {
                mask |= u32{1} << static_cast<u32>(port_index);
                m_NextDisconnected = (port_index + 1) % PortCount;
                break;
            }
        }
        return mask;
    }

    /** 未接続確認を先頭ポートから再開する。 */
    constexpr void Reset() noexcept { m_NextDisconnected = 0; }

private:
    /** 次回の未接続確認を開始するポート位置。 */
    usize m_NextDisconnected = 0;
};

} // acs::detail 名前空間
