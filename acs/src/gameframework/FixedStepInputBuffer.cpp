// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepInputBuffer.h"

namespace acs::game {

namespace {

/** platform入力が対応するゲームパッドプレイヤー数。 */
constexpr u32 kGamepadPlayerCount = 4u;

/**
 * 最新の保持状態と軸を複製し、必要なら未消費エッジを論理和で蓄積する。
 * 検証に失敗した場合はoutputを変更しない。
 */
bool TryBuildSnapshot(const IInputStateView& latest, const FInputStateSnapshot* accumulated, bool include_edges, FInputStateSnapshot& output) noexcept
{
    /** 全入力の検証が完了するまで結果を隔離するsnapshot。 */
    FInputStateSnapshot staged;

    for (usize key_index = 1u; key_index < static_cast<usize>(EKey::_Count); ++key_index) {
        /** 現在複製しているキー。 */
        const EKey key = static_cast<EKey>(key_index);
        /** 今回または未消費フレームに押されたか。 */
        const bool pressed = include_edges && (latest.IsKeyPressed(key) || (accumulated != nullptr && accumulated->IsKeyPressed(key)));
        /** 今回または未消費フレームに離されたか。 */
        const bool released = include_edges && (latest.IsKeyReleased(key) || (accumulated != nullptr && accumulated->IsKeyReleased(key)));
        if (!staged.TrySetKeyState(key, latest.IsKeyDown(key), pressed, released)) return false;
    }

    for (usize button_index = 0u; button_index < static_cast<usize>(EMouseButton::_Count); ++button_index) {
        /** 現在複製しているマウスボタン。 */
        const EMouseButton button = static_cast<EMouseButton>(button_index);
        /** 今回または未消費フレームに押されたか。 */
        const bool pressed = include_edges && (latest.IsMouseButtonPressed(button) || (accumulated != nullptr && accumulated->IsMouseButtonPressed(button)));
        /** 今回または未消費フレームに離されたか。 */
        const bool released = include_edges && (latest.IsMouseButtonReleased(button) || (accumulated != nullptr && accumulated->IsMouseButtonReleased(button)));
        if (!staged.TrySetMouseButtonState(button, latest.IsMouseButtonDown(button), pressed, released)) return false;
    }

    for (u32 player_index = 0u; player_index < kGamepadPlayerCount; ++player_index) {
        for (usize button_index = 0u; button_index < static_cast<usize>(EGamepadButton::_Count); ++button_index) {
            /** 現在複製しているゲームパッドボタン。 */
            const EGamepadButton button = static_cast<EGamepadButton>(button_index);
            /** 今回または未消費フレームに押されたか。 */
            const bool pressed = include_edges && (latest.IsGamepadButtonPressed(player_index, button) || (accumulated != nullptr && accumulated->IsGamepadButtonPressed(player_index, button)));
            /** 今回または未消費フレームに離されたか。 */
            const bool released = include_edges && (latest.IsGamepadButtonReleased(player_index, button) || (accumulated != nullptr && accumulated->IsGamepadButtonReleased(player_index, button)));
            if (!staged.TrySetGamepadButtonState(player_index, button, latest.IsGamepadButtonDown(player_index, button), pressed, released)) return false;
        }

        for (usize axis_index = 0u; axis_index < static_cast<usize>(EGamepadAxis::_Count); ++axis_index) {
            /** 現在複製しているゲームパッド軸。 */
            const EGamepadAxis axis = static_cast<EGamepadAxis>(axis_index);
            if (!staged.TrySetGamepadAxis(player_index, axis, latest.GamepadAxisValue(player_index, axis))) return false;
        }
    }

    output = staged;
    return true;
}

} // namespace

/** 一フレーム分の入力を検証し、未消費エッジと合成する。 */
bool FFixedStepInputBuffer::TryPushFrame(const IInputStateView& input) noexcept
{
    /** 既存状態を失敗から守るための合成結果。 */
    FInputStateSnapshot staged;
    /** 前回までの未消費エッジ。未初期化時は存在しない。 */
    const FInputStateSnapshot* accumulated = m_HasInputState ? &m_Pending : nullptr;
    if (!TryBuildSnapshot(input, accumulated, true, staged)) return false;

    m_Pending = staged;
    m_HasInputState = true;
    return true;
}

/** 次の固定更新用snapshotを返し、エッジだけを消費する。 */
bool FFixedStepInputBuffer::TryConsumeFixedStep(FInputStateSnapshot& output) noexcept
{
    if (!m_HasInputState) return false;

    /** エッジ消費後も次の固定更新へ引き継ぐ保持状態と軸。 */
    FInputStateSnapshot retained;
    if (!TryBuildSnapshot(m_Pending, nullptr, false, retained)) return false;

    output = m_Pending;
    m_Pending = retained;
    return true;
}

/** 未消費入力を検証し、成功時だけ保存先へ一括反映する。 */
bool FFixedStepInputBuffer::TryCaptureSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept
{
    /** 未初期化状態も正規化して保持する保存候補。 */
    FFixedStepInputBufferSnapshot candidate{};
    candidate.has_input_state = m_HasInputState;
    if (m_HasInputState && !TryBuildSnapshot(m_Pending, nullptr, true, candidate.pending_input)) return false;
    snapshot = candidate;
    return true;
}

/** 保存値を隔離したbufferで検証し、成功時だけ現在状態へ一括反映する。 */
bool FFixedStepInputBuffer::TryRestoreSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept
{
    /** 復元候補を現在状態から隔離して検証するbuffer。 */
    FFixedStepInputBuffer candidate;
    if (snapshot.has_input_state && !candidate.TryPushFrame(snapshot.pending_input)) return false;
    m_Pending = candidate.m_Pending;
    m_HasInputState = candidate.m_HasInputState;
    return true;
}

/** 蓄積中の入力を破棄し、未初期化状態へ戻す。 */
void FFixedStepInputBuffer::Reset() noexcept
{
    m_Pending.Clear();
    m_HasInputState = false;
}

/** 一度以上フレーム入力を受理している場合はtrueを返す。 */
bool FFixedStepInputBuffer::HasInputState() const noexcept
{
    return m_HasInputState;
}

} // namespace acs::game
