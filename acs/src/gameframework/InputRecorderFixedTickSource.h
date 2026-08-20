// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedTickInputSource.h"
#include "gameframework/InputRecorder.h"
#include "platform/InputCodes.h"

namespace acs::game {

/** raw key codeをACSのキーへ変換する関数。変換不能時はoutputを変更せずfalseを返す。 */
using FInputSampleKeyDecoder = bool (*)(u8 key_code, EKey& output) noexcept;

/**
 * FInputRecorderのraw sample列を固定tick入力へ変換する非所有アダプター。
 *
 * @details FInputStateSnapshotが表すキーとマウスボタンだけを変換する。mouse_posは対象外で、
 * gamepadはFInputSampleに存在しないため無入力となる。recorderとFGameのtick rateは呼び出し側で揃える。
 */
class FInputRecorderFixedTickSource final : public IFixedTickInputSource {
public:
    /**
     * 読み取り対象とraw key code変換関数を保持する。
     * @details 再生中はrecorder内容を変更しない。変更後はResetを呼ぶ。
     * @param recorder 再生するsample列。sourceより長く生存させる。
     * @param key_decoder Win32 VKやSDL scancodeを決定論的にEKeyへ変換する関数。
     */
    FInputRecorderFixedTickSource(const FInputRecorder& recorder, FInputSampleKeyDecoder key_decoder) noexcept;

    /**
     * 指定tickまでのraw変化を適用し、キーとマウスボタンのsnapshotを返す。
     * @param fixed_tick 取得する0起点tick。巻き戻し時は先頭から状態を再構築する。
     * @param output 固定更新入力。失敗時は変更しない。
     * @return sample列とkey codeを解釈できた場合はtrue。
     */
    bool TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output) noexcept override;

    /** 保持状態と読み取り位置を破棄し、次回要求を先頭から再構築する。 */
    void Reset() noexcept;

    /** recorderに保存されたtick rateを返す。 */
    u32 TickRateHz() const noexcept
    {
        return m_Recorder.TickRateHz();
    }

private:
    /** 直前要求まで適用済みのraw入力状態。 */
    struct FReplayState {
        /** キーの保持状態。 */
        bool keys_down[static_cast<usize>(EKey::_Count)]{};

        /** マウスボタンの保持bitmask。 */
        u8 mouse_buttons = 0u;

        /** 次に調べるsample index。 */
        u32 next_sample_index = 0u;

        /** 並び順検証に使う直前sampleのtick。 */
        u32 previous_sample_tick = 0u;

        /** previous_sample_tickが有効ならtrue。 */
        bool has_previous_sample = false;
    };

    /** 指定tickまでsampleを適用し、候補状態とsnapshotを構築する内部処理。 */
    bool TryBuildSnapshot_Internal(u32 fixed_tick, FReplayState& state, FInputStateSnapshot& output) const noexcept;

    /** 読み取り対象。再生中は内容を変更しない。 */
    const FInputRecorder& m_Recorder;

    /** raw key codeをEKeyへ変換する関数。 */
    FInputSampleKeyDecoder m_KeyDecoder = nullptr;

    /** 直前の連続tick要求まで適用した状態。 */
    FReplayState m_State{};

    /** 直前に成功した固定tick。 */
    u64 m_LastFixedTick = 0u;

    /** m_LastFixedTickとm_Stateが有効ならtrue。 */
    bool m_HasLastFixedTick = false;
};

} // namespace acs::game
