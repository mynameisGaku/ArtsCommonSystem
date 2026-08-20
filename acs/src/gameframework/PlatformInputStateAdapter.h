// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputStateSnapshot.h"

namespace acs::game {

/** 現在の FInput を所有された入力 snapshot へ変換する platform アダプター。 */
class FPlatformInputStateAdapter final {
public:
    /**
     * 現在のキー、マウスボタン、ゲームパッド状態を一度だけ取得する。
     *
     * 全入力をstagingへ検証してからoutputを置き換える。platformから正規範囲外の軸値を
     * 受け取った場合はfalseを返し、outputを変更しない。
     * @param output 検証済みsnapshotの書き込み先。
     * @return 全入力を取得できた場合はtrue。
     */
    static bool TryCapture(FInputStateSnapshot& output) noexcept;
};

} // namespace acs::game
