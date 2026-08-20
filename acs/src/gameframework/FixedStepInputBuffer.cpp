// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepInputBuffer.h"

namespace acs::game {
namespace {

/** platform入力が対応するゲームパッドプレイヤー数。 */
constexpr u32 kGamepadPlayerCount = 4u;

/** 最新状態を複製し、必要なら未消費の押下・解放を論理和で蓄積する。 */
bool TryBuildSnapshot(const IInputStateView& latest, const FInputStateSnapshot* accumulated, bool include_edges,
                      FInputStateSnapshot& output) noexcept
{
    FInputStateSnapshot staged;
    for (usize key_index = 1u; key_index < static_cast<usize>(EKey::_Count); ++key_index) {
        const EKey key = static_cast<EKey>(key_index);
        const bool pressed = include_edges &&
                             (latest.IsKeyPressed(key) || (accumulated != nullptr && accumulated->IsKeyPressed(key)));
        const bool released = include_edges && (latest.IsKeyReleased(key) ||
                                                (accumulated != nullptr && accumulated->IsKeyReleased(key)));
        if (!staged.TrySetKeyState(key, latest.IsKeyDown(key), pressed, released)) return false;
    }
    for (usize button_index = 0u; button_index < static_cast<usize>(EMouseButton::_Count); ++button_index) {
        const EMouseButton button = static_cast<EMouseButton>(button_index);
        const bool pressed = include_edges && (latest.IsMouseButtonPressed(button) ||
                                               (accumulated != nullptr && accumulated->IsMouseButtonPressed(button)));
        const bool released = include_edges && (latest.IsMouseButtonReleased(button) ||
                                                (accumulated != nullptr && accumulated->IsMouseButtonReleased(button)));
        if (!staged.TrySetMouseButtonState(button, latest.IsMouseButtonDown(button), pressed, released)) return false;
    }
    for (u32 player_index = 0u; player_index < kGamepadPlayerCount; ++player_index) {
        for (usize button_index = 0u; button_index < static_cast<usize>(EGamepadButton::_Count); ++button_index) {
            const EGamepadButton button = static_cast<EGamepadButton>(button_index);
            const bool pressed = include_edges && (latest.IsGamepadButtonPressed(player_index, button) ||
                                                   (accumulated != nullptr &&
                                                    accumulated->IsGamepadButtonPressed(player_index, button)));
            const bool released = include_edges && (latest.IsGamepadButtonReleased(player_index, button) ||
                                                    (accumulated != nullptr &&
                                                     accumulated->IsGamepadButtonReleased(player_index, button)));
            if (!staged.TrySetGamepadButtonState(player_index, button, latest.IsGamepadButtonDown(player_index, button),
                                                 pressed, released))
                return false;
        }
        for (usize axis_index = 0u; axis_index < static_cast<usize>(EGamepadAxis::_Count); ++axis_index) {
            const EGamepadAxis axis = static_cast<EGamepadAxis>(axis_index);
            if (!staged.TrySetGamepadAxis(player_index, axis, latest.GamepadAxisValue(player_index, axis)))
                return false;
        }
    }
    output = staged;
    return true;
}

} // namespace

bool FFixedStepInputBuffer::TryPushFrame(const IInputStateView& input) noexcept
{
    FInputStateSnapshot staged;
    const FInputStateSnapshot* accumulated = m_HasInputState ? &m_Pending : nullptr;
    if (!TryBuildSnapshot(input, accumulated, true, staged)) return false;
    m_Pending = staged;
    m_HasInputState = true;
    return true;
}

bool FFixedStepInputBuffer::TryConsumeFixedStep(FInputStateSnapshot& output) noexcept
{
    if (!m_HasInputState) return false;
    FInputStateSnapshot retained;
    if (!TryBuildSnapshot(m_Pending, nullptr, false, retained)) return false;
    output = m_Pending;
    m_Pending = retained;
    return true;
}

bool FFixedStepInputBuffer::TryCaptureSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept
{
    FFixedStepInputBufferSnapshot candidate{};
    candidate.has_input_state = m_HasInputState;
    if (m_HasInputState && !TryBuildSnapshot(m_Pending, nullptr, true, candidate.pending_input)) return false;
    snapshot = candidate;
    return true;
}

bool FFixedStepInputBuffer::TryRestoreSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept
{
    FFixedStepInputBuffer candidate;
    if (snapshot.has_input_state && !candidate.TryPushFrame(snapshot.pending_input)) return false;
    m_Pending = candidate.m_Pending;
    m_HasInputState = candidate.m_HasInputState;
    return true;
}

void FFixedStepInputBuffer::Reset() noexcept
{
    m_Pending.Clear();
    m_HasInputState = false;
}

bool FFixedStepInputBuffer::HasInputState() const noexcept
{
    return m_HasInputState;
}

} // namespace acs::game
