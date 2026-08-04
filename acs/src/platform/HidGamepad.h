// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"

// XInput が拾わないゲームパッドを HID から直接読む。
//
// XInput は Xbox 系専用のため、PlayStation 系 (DualShock 4 / DualSense) と Nintendo 系
// (Pro Controller / Joy-Con) は XInput から見えない。DirectInput でも一応は読めるが、軸の
// 割り当てが機種ごとに推測になり、Switch 系は USB だと初期化しないと標準レポートを出さない。
//
// そこで JoyShockLibrary と同じく HID を直接開き、機種ごとの入力レポートを自前で解釈する。
// 各機種の識別は VID/PID で行い、ボタンは刻印ではなく物理位置 (South / East / West / North)
// へ読み替えるので、Nintendo のように A と B が左右逆な機種でも呼び出し側は同じ扱いで済む。
//
// 対応:
//   ・DualShock 4 (USB / Bluetooth)
//   ・DualSense (USB / Bluetooth)
//   ・Switch Pro Controller / Joy-Con (USB は 0x80 系のハンドシェイク後、Bluetooth はそのまま)
//
// CInput が内部で使う想定で、ゲーム側は CInput のゲームパッド API を通して触る。

namespace acs {

/**
 * HID から読んだゲームパッド 1 台分の状態。
 *
 * @details XInput の生状態とは独立した、機種差を吸収済みの表現。
 */
struct FHidGamepadState {
    /** 押下中のボタン (EGamepadButton を添字とするビット)。 */
    u32 buttons = 0;

    /** 各軸の値 (スティックは -1..+1、トリガーは 0..1)。 */
    f32 axes[static_cast<usize>(EGamepadAxis::_Count)] {};

    /** このスロットにデバイスが繋がっているか。 */
    bool connected = false;
};

/**
 * 対応している機種の種別。
 */
enum class EHidGamepadKind : u8 {
    /** 未対応 / 未接続。 */
    None,

    /** DualShock 4。 */
    DualShock4,

    /** DualSense。 */
    DualSense,

    /** Switch Pro Controller / 充電グリップ。 */
    SwitchPro,

    /** Joy-Con (L)。 */
    JoyConLeft,

    /** Joy-Con (R)。 */
    JoyConRight,
};

/**
 * HID のゲームパッドを列挙して読む源。
 *
 * @details
 * 実体の寿命は利用側 (CInput) が持つ。Poll をフレーム先頭で 1 回呼ぶと、接続済みデバイスの
 * 入力レポートを読んで状態を更新し、一定間隔でデバイスの再列挙も行う (ホットプラグ対応)。
 */
class CHidGamepadSource {
public:
    /** 同時に扱うデバイス数の上限。 */
    static constexpr u32 kMaxDevices = 4;

    /** 空の状態で構築する (デバイスは最初の Poll で探す)。 */
    CHidGamepadSource() noexcept = default;

    /** 開いているデバイスを閉じる。 */
    ~CHidGamepadSource() noexcept;

    /** コピー禁止 (デバイスハンドルを単独所有するため)。 */
    CHidGamepadSource(const CHidGamepadSource&) = delete;

    /** コピー代入も禁止。 */
    CHidGamepadSource& operator=(const CHidGamepadSource&) = delete;

    /**
     * 全デバイスの状態を取得する (フレーム先頭で 1 回呼ぶ)。
     *
     * @details 初回と一定間隔で再列挙し、以降は開いているデバイスからレポートを読む。
     */
    void Poll() noexcept;

    /**
     * 指定スロットの状態を返す。
     *
     * @param index 0..kMaxDevices-1 のスロット番号。
     * @return そのスロットの状態 (範囲外や未接続なら connected == false)。
     */
    const FHidGamepadState& GetState(u32 index) const noexcept;

    /**
     * 指定スロットの機種を返す。
     *
     * @param index 0..kMaxDevices-1 のスロット番号。
     * @return 機種 (未接続なら None)。
     */
    EHidGamepadKind GetKind(u32 index) const noexcept;

    /** 開いているデバイスを全て閉じる。 */
    void Shutdown() noexcept;

private:
    /** 開いているデバイス 1 台分。 */
    struct FDevice {
        /** デバイスハンドル (未使用なら無効値)。 */
        void* handle = nullptr;

        /** 読み取り用の重なり I/O 構造体 (実装側で確保する)。 */
        void* overlapped = nullptr;

        /** 入力レポートの受け皿。 */
        u8 report[64] {};

        /** 1 回の入力レポート長。 */
        u32 report_length = 0;

        /** デバイスパス (再列挙時の重複判定に使う)。 */
        char path[256] {};

        /** 機種。 */
        EHidGamepadKind kind = EHidGamepadKind::None;

        /** Bluetooth 接続か (レポートのオフセットが変わる)。 */
        bool is_bluetooth = false;

        /** 読み取りを発行済みか (重なり I/O の完了待ち)。 */
        bool read_pending = false;

        /** Switch 系の出力レポートに付ける通し番号 (0..15 で回す)。 */
        u8 packet_number = 0;
    };

    /**
     * 接続済みデバイスを探し直す。
     *
     * @details 既に開いているものは開き直さず、消えたものだけ閉じる。
     */
    void Rescan() noexcept;

    /**
     * 1 台ぶんの入力レポートを読んで状態へ反映する。
     *
     * @param slot 対象スロット。
     */
    void ReadDevice(u32 slot) noexcept;

    /** 各スロットのデバイス。 */
    FDevice m_Devices[kMaxDevices] {};

    /** 各スロットの状態。 */
    FHidGamepadState m_States[kMaxDevices] {};

    /** 未接続スロットを返すための既定値。 */
    FHidGamepadState m_Empty {};

    /** 次に再列挙するまでに残っている Poll 回数。 */
    u32 m_RescanCountdown = 0;
};

} // namespace acs
