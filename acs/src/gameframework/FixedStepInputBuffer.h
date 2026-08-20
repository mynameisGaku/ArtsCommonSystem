// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputStateSnapshot.h"

namespace acs::game {

/**
 * 可変フレーム入力を固定更新ごとのsnapshotへ変換する状態保持器。
 *
 * 保持状態と軸は最新フレームを採用し、押下・解放は次の固定更新まで蓄積する。
 * 固定更新へ渡した押下・解放は一度だけ消費し、以後のcatch-up更新では再送しない。
 */
class FFixedStepInputBuffer final {
public:
    /**
     * 一フレーム分の入力を蓄積する。
     * @param input platform、replay、AIなどから得た入力状態。
     * @return 全入力が有効で蓄積できた場合はtrue。不正な軸値では既存状態を保つ。
     */
    bool TryPushFrame(const IInputStateView& input) noexcept;

    /**
     * 次の固定更新へ渡す入力を取得し、押下・解放だけを消費する。
     * @param output 固定更新で使うsnapshot。未初期化または失敗時は変更しない。
     * @return 一度以上フレーム入力を受理済みで取得できた場合はtrue。
     */
    bool TryConsumeFixedStep(FInputStateSnapshot& output) noexcept;

    /** 蓄積中の入力を破棄し、未初期化状態へ戻す。 */
    void Reset() noexcept;

    /** 一度以上フレーム入力を受理している場合はtrueを返す。 */
    bool HasInputState() const noexcept;

private:
    /** 次の固定更新へ渡す保持状態、軸、未消費エッジ。 */
    FInputStateSnapshot m_Pending;

    /** m_Pendingが受理済み入力を表す場合はtrue。 */
    bool m_HasInputState = false;
};

} // namespace acs::game
