// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputStateSnapshot.h"

namespace acs::game {

/** 現在のCInputを所有された入力snapshotへ変換するplatformアダプター。 */
class CPlatformInputStateAdapter final {
public:
    /**
     * 現在のキー、マウスボタン、ゲームパッド状態を一度だけ取得する。
     *
     * 全入力を一時値へ検証してからoutputを置き換える。不正な軸値ではoutputを変更しない。
     */
    static bool TryCapture(FInputStateSnapshot& output) noexcept;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FPlatformInputStateAdapter = CPlatformInputStateAdapter;

} // namespace acs::game
