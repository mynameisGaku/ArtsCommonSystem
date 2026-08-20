// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedStepInputBufferSnapshot.h"

namespace acs::game {

/**
 * 可変フレーム入力を固定更新ごとのsnapshotへ変換する状態保持器。
 *
 * 保持状態と軸は最新フレームを採用し、押下・解放は次の固定更新まで蓄積する。
 * 固定更新へ渡した押下・解放は一度だけ消費し、以後のcatch-up更新では再送しない。
 */
class FFixedStepInputBuffer final {
public:
    /** 一フレーム分の入力を蓄積し、不正な値では既存状態を保つ。 */
    bool TryPushFrame(const IInputStateView& input) noexcept;

    /** 次の固定更新へ渡す入力を取得し、押下・解放だけを消費する。 */
    bool TryConsumeFixedStep(FInputStateSnapshot& output) noexcept;

    /** 未消費入力を再現可能な保存値へ複製する。 */
    bool TryCaptureSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept;

    /** 保存した未消費入力を検証して復元し、失敗時は現在状態を保つ。 */
    bool TryRestoreSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept;

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

/** 旧名を使う既存コード向けの互換別名。 */
using CFixedStepInputBuffer = FFixedStepInputBuffer;

} // namespace acs::game
