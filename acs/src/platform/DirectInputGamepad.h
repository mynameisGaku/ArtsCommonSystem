// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"

// XInput にも HID の機種別対応にも載らないゲームパッドを DirectInput から読む。
//
// Xbox 系は XInput、PlayStation 系と Nintendo 系は platform/HidGamepad が受け持つ。ここは
// その 2 つから漏れる汎用パッド (サードパーティ製のアーケードスティックや古いパッド等) を
// 拾うための最後の受け皿。
//
// DirectInput は軸やボタンの並びを機器が自由に決められるため、どのボタンがどの位置かは
// 分からない。DirectInput の慣習 (0 番から順に South / East / West / North) として扱い、
// 十字キーは POV から取る。XInput で既に見えている機器は二重に数えないよう除外する。

namespace acs {

/**
 * DirectInput 経由で読んだゲームパッド 1 台分の状態。
 */
struct FDirectInputGamepadState {
    /** 押下中のボタン (EGamepadButton を添字とするビット)。 */
    u32 buttons = 0;

    /** 各軸の値 (スティックは -1..+1、トリガーは 0..1)。 */
    f32 axes[static_cast<usize>(EGamepadAxis::_Count)] {};

    /** このスロットにデバイスが繋がっているか。 */
    bool connected = false;
};

/**
 * DirectInput のゲームパッドを列挙して読む源。
 *
 * @details
 * 実体の寿命は利用側 (CInput) が持つ。Poll をフレーム先頭で 1 回呼ぶと状態を更新し、
 * 一定間隔でデバイスを列挙し直す (ホットプラグ対応)。DirectInput が使えない環境では
 * 初期化に 1 度だけ失敗し、以降は何もしない。
 */
class CDirectInputGamepadSource {
public:
    /** 同時に扱うデバイス数の上限。 */
    static constexpr u32 kMaxDevices = 4;

    /** 空の状態で構築する (初期化は最初の Poll で行う)。 */
    CDirectInputGamepadSource() noexcept = default;

    /** 保持しているデバイスを解放する。 */
    ~CDirectInputGamepadSource() noexcept;

    /** コピー禁止 (デバイスを単独所有するため)。 */
    CDirectInputGamepadSource(const CDirectInputGamepadSource&) = delete;

    /** コピー代入も禁止。 */
    CDirectInputGamepadSource& operator=(const CDirectInputGamepadSource&) = delete;

    /**
     * 全デバイスの状態を取得する (フレーム先頭で 1 回呼ぶ)。
     */
    void Poll() noexcept;

    /**
     * 指定スロットの状態を返す。
     *
     * @param index 0..kMaxDevices-1 のスロット番号。
     * @return そのスロットの状態 (範囲外や未接続なら connected == false)。
     */
    const FDirectInputGamepadState& GetState(u32 index) const noexcept;

    /** 保持しているデバイスを全て解放する。 */
    void Shutdown() noexcept;

private:
    /** 開いているデバイス 1 台分。 */
    struct FDevice {
        /** IDirectInputDevice8W* (型を漏らさないため void* で持つ)。 */
        void* device = nullptr;

        /** 機器を識別する GUID (再列挙時の重複判定に使う)。 */
        u8 instance_guid[16] {};
    };

    /** 接続済みデバイスを探し直す。 */
    void Rescan() noexcept;

    /** 各スロットのデバイス。 */
    FDevice m_Devices[kMaxDevices] {};

    /** 各スロットの状態。 */
    FDirectInputGamepadState m_States[kMaxDevices] {};

    /** 未接続スロットを返すための既定値。 */
    FDirectInputGamepadState m_Empty {};

    /** IDirectInput8W* (型を漏らさないため void* で持つ)。 */
    void* m_DirectInput = nullptr;

    /** 次に再列挙するまでに残っている Poll 回数。 */
    u32 m_RescanCountdown = 0;

    /** 初期化を試したか (失敗した場合も二度と試さない)。 */
    bool m_Tried = false;
};

} // namespace acs
