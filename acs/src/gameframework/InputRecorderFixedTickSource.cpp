// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputRecorderFixedTickSource.h"

#include "gameframework/InputStateSnapshot.h"

namespace acs::game {

namespace {

/** FInputSampleが表現できるマウスボタンbit。 */
constexpr u8 kSupportedMouseButtons = static_cast<u8>((1u << static_cast<u8>(EMouseButton::_Count)) - 1u);

/** snapshotへ設定できる実キーならtrue。 */
bool IsValidKey(EKey key) noexcept
{
    const usize index = static_cast<usize>(key);
    return index > static_cast<usize>(EKey::Unknown) && index < static_cast<usize>(EKey::_Count);
}

} // namespace

/** recorderとkey decoderを非所有で保持する。 */
FInputRecorderFixedTickSource::FInputRecorderFixedTickSource(const FInputRecorder& recorder,
                                                             FInputSampleKeyDecoder key_decoder) noexcept
    : m_Recorder(recorder), m_KeyDecoder(key_decoder)
{
}

/** 保持状態と読み取り位置を初期化する。 */
void FInputRecorderFixedTickSource::Reset() noexcept
{
    m_State = FReplayState{};
    m_LastFixedTick = 0u;
    m_HasLastFixedTick = false;
}

/** raw sample列を指定tickまで適用し、現在tickだけのedgeを持つsnapshotを構築する。 */
bool FInputRecorderFixedTickSource::TryBuildSnapshot_Internal(u32 fixed_tick, FReplayState& state,
                                                              FInputStateSnapshot& output) const noexcept
{
    /** 現在tickで押されたキー。 */
    bool keys_pressed[static_cast<usize>(EKey::_Count)]{};
    /** 現在tickで離されたキー。 */
    bool keys_released[static_cast<usize>(EKey::_Count)]{};
    /** 現在tickで押されたマウスボタンbit。 */
    u8 mouse_pressed = 0u;
    /** 現在tickで離されたマウスボタンbit。 */
    u8 mouse_released = 0u;

    const FInputRecorder::FReplayReadAdapter reader = m_Recorder.ReplayReadAccess();
    const u32 sample_count = m_Recorder.SampleCount();
    while (state.next_sample_index < sample_count) {
        FInputSample sample{};
        if (!reader.TryReadSampleAt(state.next_sample_index, sample)) return false;
        if (state.has_previous_sample && sample.tick < state.previous_sample_tick) return false;
        if (sample.tick > fixed_tick) break;
        if ((sample.mouse_button_states & static_cast<u8>(~kSupportedMouseButtons)) != 0u) return false;

        /** このsampleの変化を現在tickのedgeとして公開する場合はtrue。 */
        const bool report_edges = sample.tick == fixed_tick;
        for (usize slot = 0u; slot < 8u; ++slot) {
            const u8 key_code = sample.key_codes_changed[slot];
            const u8 key_state = sample.key_states[slot];
            if (key_code == 0u) {
                if (key_state != 0u) return false;
                continue;
            }
            if (key_state > 1u || m_KeyDecoder == nullptr) return false;

            EKey key = EKey::Unknown;
            if (!m_KeyDecoder(key_code, key) || !IsValidKey(key)) return false;
            const usize key_index = static_cast<usize>(key);
            const bool down = key_state != 0u;
            if (down != state.keys_down[key_index] && report_edges) {
                if (down)
                    keys_pressed[key_index] = true;
                else
                    keys_released[key_index] = true;
            }
            state.keys_down[key_index] = down;
        }

        if (report_edges) {
            mouse_pressed |= static_cast<u8>(sample.mouse_button_states & static_cast<u8>(~state.mouse_buttons));
            mouse_released |= static_cast<u8>(state.mouse_buttons & static_cast<u8>(~sample.mouse_button_states));
        }
        state.mouse_buttons = sample.mouse_button_states;
        state.previous_sample_tick = sample.tick;
        state.has_previous_sample = true;
        ++state.next_sample_index;
    }

    FInputStateSnapshot staged;
    for (usize key_index = static_cast<usize>(EKey::Unknown) + 1u; key_index < static_cast<usize>(EKey::_Count);
         ++key_index) {
        const EKey key = static_cast<EKey>(key_index);
        if (!staged.TrySetKeyState(key, state.keys_down[key_index], keys_pressed[key_index], keys_released[key_index]))
            return false;
    }
    for (usize button_index = 0u; button_index < static_cast<usize>(EMouseButton::_Count); ++button_index) {
        const EMouseButton button = static_cast<EMouseButton>(button_index);
        const u8 button_bit = static_cast<u8>(1u << button_index);
        if (!staged.TrySetMouseButtonState(button, (state.mouse_buttons & button_bit) != 0u,
                                           (mouse_pressed & button_bit) != 0u, (mouse_released & button_bit) != 0u))
            return false;
    }
    output = staged;
    return true;
}

/** 連続要求はcached stateを進め、seekまたはrollback要求は先頭から再構築する。 */
bool FInputRecorderFixedTickSource::TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output) noexcept
{
    if (fixed_tick > static_cast<u64>(~u32{0}) || m_KeyDecoder == nullptr) return false;

    FReplayState staged_state{};
    if (m_HasLastFixedTick && m_LastFixedTick < ~u64{0} && fixed_tick == m_LastFixedTick + 1u) {
        staged_state = m_State;
    }
    FInputStateSnapshot staged_output;
    if (!TryBuildSnapshot_Internal(static_cast<u32>(fixed_tick), staged_state, staged_output)) return false;

    m_State = staged_state;
    m_LastFixedTick = fixed_tick;
    m_HasLastFixedTick = true;
    output = staged_output;
    return true;
}

} // namespace acs::game
