// SPDX-License-Identifier: Apache-2.0
// HID ゲームパッド読み取り実装 (PlayStation 系 / Nintendo 系)

#include "platform/HidGamepad.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace acs {

namespace {

/** Sony のベンダー ID。 */
constexpr u16 kVendorSony = 0x054Cu;

/** Nintendo のベンダー ID。 */
constexpr u16 kVendorNintendo = 0x057Eu;

/** DualShock 4 (初代)。 */
constexpr u16 kProductDs4V1 = 0x05C4u;

/** DualShock 4 (2016 年以降)。 */
constexpr u16 kProductDs4V2 = 0x09CCu;

/** DualShock 4 USB ドングル。 */
constexpr u16 kProductDs4Dongle = 0x0BA0u;

/** DualSense。 */
constexpr u16 kProductDualSense = 0x0CE6u;

/** DualSense Edge。 */
constexpr u16 kProductDualSenseEdge = 0x0DF2u;

/** Joy-Con (L)。 */
constexpr u16 kProductJoyConLeft = 0x2006u;

/** Joy-Con (R)。 */
constexpr u16 kProductJoyConRight = 0x2007u;

/** Pro Controller。 */
constexpr u16 kProductSwitchPro = 0x2009u;

/** 充電グリップ (Joy-Con を挿した状態)。 */
constexpr u16 kProductChargingGrip = 0x200Eu;

/** 再列挙するまでの Poll 回数 (毎フレーム列挙すると重いので間引く)。 */
constexpr u32 kRescanInterval = 120;

/** スティックの中央付近を切り捨てる幅 (0..1)。 */
constexpr f32 kStickDeadzone = 0.15f;

/** Switch のスティック生値の中心 (12 bit の中央)。 */
constexpr f32 kSwitchStickCenter = 2048.0f;

/** Switch のスティック生値の振れ幅。 */
constexpr f32 kSwitchStickRange = 1800.0f;

/**
 * ボタンビットを立てる。
 *
 * @param buttons 立てる先。
 * @param button 対象のボタン。
 * @param pressed 押されていれば true。
 */
inline void SetButton(u32& buttons, EGamepadButton button, bool pressed) noexcept {
    if (!pressed) return;
    buttons |= (u32{1} << static_cast<u32>(button));
}

/**
 * 0..255 の軸生値を -1..+1 へ正規化する。
 *
 * @param raw 生値 (128 が中央)。
 * @return デッドゾーン処理済みの値。
 */
f32 NormalizeByteAxis(u8 raw) noexcept {
    /** -1..+1 へ写した値。 */
    f32 value = (static_cast<f32>(raw) - 127.5f) / 127.5f;
    if (value > -kStickDeadzone && value < kStickDeadzone) return 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return value;
}

/**
 * Switch の 12 bit スティック生値を -1..+1 へ正規化する。
 *
 * @param raw 生値 (0..4095)。
 * @return デッドゾーン処理済みの値。
 */
f32 NormalizeSwitchAxis(u32 raw) noexcept {
    /** -1..+1 へ写した値。 */
    f32 value = (static_cast<f32>(raw) - kSwitchStickCenter) / kSwitchStickRange;
    if (value > -kStickDeadzone && value < kStickDeadzone) return 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return value;
}

/**
 * 十字キーの hat 値 (0=上、時計回り、8=なし) を方向ボタンへ展開する。
 *
 * @param buttons 立てる先。
 * @param hat hat 値。
 */
void ApplyHat(u32& buttons, u8 hat) noexcept {
    if (hat > 7) return;

    SetButton(buttons, EGamepadButton::Up,    hat == 7 || hat == 0 || hat == 1);
    SetButton(buttons, EGamepadButton::Right, hat == 1 || hat == 2 || hat == 3);
    SetButton(buttons, EGamepadButton::Down,  hat == 3 || hat == 4 || hat == 5);
    SetButton(buttons, EGamepadButton::Left,  hat == 5 || hat == 6 || hat == 7);
}

/**
 * VID/PID から機種を判定する。
 *
 * @param vendor ベンダー ID。
 * @param product プロダクト ID。
 * @return 判定した機種 (未対応なら None)。
 */
EHidGamepadKind ClassifyDevice(u16 vendor, u16 product) noexcept {
    if (vendor == kVendorSony) {
        if (product == kProductDs4V1 || product == kProductDs4V2 || product == kProductDs4Dongle) {
            return EHidGamepadKind::DualShock4;
        }
        if (product == kProductDualSense || product == kProductDualSenseEdge) {
            return EHidGamepadKind::DualSense;
        }
        return EHidGamepadKind::None;
    }

    if (vendor == kVendorNintendo) {
        if (product == kProductSwitchPro || product == kProductChargingGrip) return EHidGamepadKind::SwitchPro;
        if (product == kProductJoyConLeft) return EHidGamepadKind::JoyConLeft;
        if (product == kProductJoyConRight) return EHidGamepadKind::JoyConRight;
    }
    return EHidGamepadKind::None;
}

/**
 * DualShock 4 の入力レポートを解釈する。
 *
 * @details USB はレポート ID 0x01、Bluetooth は 0x11 で 2 byte ぶん後ろへずれる。
 * @param report 入力レポート。
 * @param length レポート長。
 * @param out_state 書き込み先。
 * @return 解釈できたら true。
 */
bool ParseDualShock4(const u8* report, u32 length, FHidGamepadState& out_state) noexcept {
    /** ボタンやスティックが始まる位置。 */
    u32 base = 0;
    if (report[0] == 0x01u) base = 1;
    else if (report[0] == 0x11u) base = 3;
    else return false;

    if (length < base + 9) return false;

    u32 buttons = 0;
    /** 十字キーと face ボタンをまとめた byte。 */
    const u8 face = report[base + 4];
    ApplyHat(buttons, static_cast<u8>(face & 0x0Fu));
    SetButton(buttons, EGamepadButton::West,  (face & 0x10u) != 0);   // □
    SetButton(buttons, EGamepadButton::South, (face & 0x20u) != 0);   // ×
    SetButton(buttons, EGamepadButton::East,  (face & 0x40u) != 0);   // ○
    SetButton(buttons, EGamepadButton::North, (face & 0x80u) != 0);   // △

    /** 肩ボタンとスティック押し込みをまとめた byte。 */
    const u8 shoulder = report[base + 5];
    SetButton(buttons, EGamepadButton::LeftBumper,  (shoulder & 0x01u) != 0);
    SetButton(buttons, EGamepadButton::RightBumper, (shoulder & 0x02u) != 0);
    SetButton(buttons, EGamepadButton::Back,        (shoulder & 0x10u) != 0);   // Share
    SetButton(buttons, EGamepadButton::Start,       (shoulder & 0x20u) != 0);   // Options
    SetButton(buttons, EGamepadButton::LeftStick,   (shoulder & 0x40u) != 0);
    SetButton(buttons, EGamepadButton::RightStick,  (shoulder & 0x80u) != 0);

    SetButton(buttons, EGamepadButton::Guide, (report[base + 6] & 0x01u) != 0);  // PS

    out_state.buttons = buttons;
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)]  = NormalizeByteAxis(report[base + 0]);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)]  = -NormalizeByteAxis(report[base + 1]);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightX)] = NormalizeByteAxis(report[base + 2]);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightY)] = -NormalizeByteAxis(report[base + 3]);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftTrigger)]  = static_cast<f32>(report[base + 7]) / 255.0f;
    out_state.axes[static_cast<usize>(EGamepadAxis::RightTrigger)] = static_cast<f32>(report[base + 8]) / 255.0f;
    return true;
}

/**
 * DualSense の入力レポートを解釈する。
 *
 * @details USB はレポート ID 0x01、Bluetooth は 0x31 で 1 byte ぶん後ろへずれる。
 * @param report 入力レポート。
 * @param length レポート長。
 * @param out_state 書き込み先。
 * @return 解釈できたら true。
 */
bool ParseDualSense(const u8* report, u32 length, FHidGamepadState& out_state) noexcept {
    /** スティックが始まる位置。 */
    u32 base = 0;
    if (report[0] == 0x01u) base = 1;
    else if (report[0] == 0x31u) base = 2;
    else return false;

    if (length < base + 10) return false;

    u32 buttons = 0;
    /** 十字キーと face ボタンをまとめた byte。 */
    const u8 face = report[base + 7];
    ApplyHat(buttons, static_cast<u8>(face & 0x0Fu));
    SetButton(buttons, EGamepadButton::West,  (face & 0x10u) != 0);   // □
    SetButton(buttons, EGamepadButton::South, (face & 0x20u) != 0);   // ×
    SetButton(buttons, EGamepadButton::East,  (face & 0x40u) != 0);   // ○
    SetButton(buttons, EGamepadButton::North, (face & 0x80u) != 0);   // △

    /** 肩ボタンとスティック押し込みをまとめた byte。 */
    const u8 shoulder = report[base + 8];
    SetButton(buttons, EGamepadButton::LeftBumper,  (shoulder & 0x01u) != 0);
    SetButton(buttons, EGamepadButton::RightBumper, (shoulder & 0x02u) != 0);
    SetButton(buttons, EGamepadButton::Back,        (shoulder & 0x10u) != 0);   // Create
    SetButton(buttons, EGamepadButton::Start,       (shoulder & 0x20u) != 0);   // Options
    SetButton(buttons, EGamepadButton::LeftStick,   (shoulder & 0x40u) != 0);
    SetButton(buttons, EGamepadButton::RightStick,  (shoulder & 0x80u) != 0);

    SetButton(buttons, EGamepadButton::Guide, (report[base + 9] & 0x01u) != 0);  // PS

    out_state.buttons = buttons;
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)]  = NormalizeByteAxis(report[base + 0]);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)]  = -NormalizeByteAxis(report[base + 1]);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightX)] = NormalizeByteAxis(report[base + 2]);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightY)] = -NormalizeByteAxis(report[base + 3]);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftTrigger)]  = static_cast<f32>(report[base + 4]) / 255.0f;
    out_state.axes[static_cast<usize>(EGamepadAxis::RightTrigger)] = static_cast<f32>(report[base + 5]) / 255.0f;
    return true;
}

/**
 * Switch 系の標準入力レポート (0x30) を解釈する。
 *
 * @details
 * 刻印ではなく物理位置へ写す。Nintendo は A が右、B が下、X が上、Y が左なので、
 * そのまま East / South / North / West になる。
 * @param report 入力レポート。
 * @param length レポート長。
 * @param kind 機種 (Joy-Con 単体は倒す向きが 90 度違う)。
 * @param out_state 書き込み先。
 * @return 解釈できたら true。
 */
bool ParseSwitch(const u8* report, u32 length, EHidGamepadKind kind, FHidGamepadState& out_state) noexcept {
    if (report[0] != 0x30u && report[0] != 0x21u) return false;
    if (length < 12) return false;

    u32 buttons = 0;

    /** 右側 (face ボタンと R / ZR)。 */
    const u8 right = report[3];
    SetButton(buttons, EGamepadButton::West,  (right & 0x01u) != 0);   // Y
    SetButton(buttons, EGamepadButton::North, (right & 0x02u) != 0);   // X
    SetButton(buttons, EGamepadButton::South, (right & 0x04u) != 0);   // B
    SetButton(buttons, EGamepadButton::East,  (right & 0x08u) != 0);   // A
    SetButton(buttons, EGamepadButton::RightBumper, (right & 0x40u) != 0);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightTrigger)] = ((right & 0x80u) != 0) ? 1.0f : 0.0f;

    /** 中央 (Minus / Plus / スティック押し込み / Home / Capture)。 */
    const u8 shared = report[4];
    SetButton(buttons, EGamepadButton::Back,       (shared & 0x01u) != 0);   // -
    SetButton(buttons, EGamepadButton::Start,      (shared & 0x02u) != 0);   // +
    SetButton(buttons, EGamepadButton::RightStick, (shared & 0x04u) != 0);
    SetButton(buttons, EGamepadButton::LeftStick,  (shared & 0x08u) != 0);
    SetButton(buttons, EGamepadButton::Guide,      (shared & 0x10u) != 0);   // Home

    /** 左側 (十字キーと L / ZL)。 */
    const u8 left = report[5];
    SetButton(buttons, EGamepadButton::Down,  (left & 0x01u) != 0);
    SetButton(buttons, EGamepadButton::Up,    (left & 0x02u) != 0);
    SetButton(buttons, EGamepadButton::Right, (left & 0x04u) != 0);
    SetButton(buttons, EGamepadButton::Left,  (left & 0x08u) != 0);
    SetButton(buttons, EGamepadButton::LeftBumper, (left & 0x40u) != 0);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftTrigger)] = ((left & 0x80u) != 0) ? 1.0f : 0.0f;

    out_state.buttons = buttons;

    // スティックは 12 bit 2 本を 3 byte へ詰めてある。
    const u32 left_x  = static_cast<u32>(report[6]) | ((static_cast<u32>(report[7]) & 0x0Fu) << 8);
    const u32 left_y  = (static_cast<u32>(report[7]) >> 4) | (static_cast<u32>(report[8]) << 4);
    const u32 right_x = static_cast<u32>(report[9]) | ((static_cast<u32>(report[10]) & 0x0Fu) << 8);
    const u32 right_y = (static_cast<u32>(report[10]) >> 4) | (static_cast<u32>(report[11]) << 4);

    out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)]  = NormalizeSwitchAxis(left_x);
    out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)]  = NormalizeSwitchAxis(left_y);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightX)] = NormalizeSwitchAxis(right_x);
    out_state.axes[static_cast<usize>(EGamepadAxis::RightY)] = NormalizeSwitchAxis(right_y);

    // Joy-Con 単体は横持ちが前提で、スティックの向きが本体と 90 度ずれる。
    if (kind == EHidGamepadKind::JoyConLeft) {
        const f32 x = out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)];
        const f32 y = out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)];
        out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)] = -y;
        out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)] = x;
    } else if (kind == EHidGamepadKind::JoyConRight) {
        const f32 x = out_state.axes[static_cast<usize>(EGamepadAxis::RightX)];
        const f32 y = out_state.axes[static_cast<usize>(EGamepadAxis::RightY)];
        out_state.axes[static_cast<usize>(EGamepadAxis::LeftX)] = y;
        out_state.axes[static_cast<usize>(EGamepadAxis::LeftY)] = -x;
    }
    return true;
}

/**
 * Switch へ 0x80 系のコマンドを送る (USB 接続時のハンドシェイクに使う)。
 *
 * @param handle デバイスハンドル。
 * @param command コマンド番号。
 */
void SendSwitchUsbCommand(HANDLE handle, u8 command) noexcept {
    u8 buffer[64] {};
    buffer[0] = 0x80u;
    buffer[1] = command;

    DWORD written = 0;
    OVERLAPPED overlapped {};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return;

    if (!::WriteFile(handle, buffer, sizeof(buffer), &written, &overlapped)) {
        if (::GetLastError() == ERROR_IO_PENDING) {
            // 応答しない機種で止まらないよう、短い時間だけ待って諦める。
            ::WaitForSingleObject(overlapped.hEvent, 100);
        }
    }
    ::CancelIo(handle);
    ::CloseHandle(overlapped.hEvent);
}

/**
 * Switch へ subcommand を送る。
 *
 * @param handle デバイスハンドル。
 * @param packet_number 0..15 で回す通し番号 (呼び出しごとに進める)。
 * @param subcommand subcommand 番号。
 * @param argument subcommand の引数 1 byte。
 */
void SendSwitchSubcommand(HANDLE handle, u8& packet_number, u8 subcommand, u8 argument) noexcept {
    u8 buffer[64] {};
    buffer[0] = 0x01u;                                   // Rumble + subcommand
    buffer[1] = static_cast<u8>(packet_number & 0x0Fu);
    packet_number = static_cast<u8>(packet_number + 1);

    // 振動は無効のまま (中立値)。
    buffer[2] = 0x00u; buffer[3] = 0x01u; buffer[4] = 0x40u; buffer[5] = 0x40u;
    buffer[6] = 0x00u; buffer[7] = 0x01u; buffer[8] = 0x40u; buffer[9] = 0x40u;

    buffer[10] = subcommand;
    buffer[11] = argument;

    DWORD written = 0;
    OVERLAPPED overlapped {};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return;

    if (!::WriteFile(handle, buffer, sizeof(buffer), &written, &overlapped)) {
        if (::GetLastError() == ERROR_IO_PENDING) {
            ::WaitForSingleObject(overlapped.hEvent, 100);
        }
    }
    ::CancelIo(handle);
    ::CloseHandle(overlapped.hEvent);
}

/**
 * Switch 系を標準レポートモードへ入れる。
 *
 * @details
 * USB 接続では 0x80 系のハンドシェイクを先に通す必要がある。Bluetooth 接続では不要だが、
 * 送っても無視されるだけなので、接続方式を判定せず両方送る。
 * @param handle デバイスハンドル。
 * @param packet_number 通し番号 (進められる)。
 */
void InitializeSwitch(HANDLE handle, u8& packet_number) noexcept {
    SendSwitchUsbCommand(handle, 0x01u);   // 機器情報 (ボーレート取得)
    SendSwitchUsbCommand(handle, 0x02u);   // ハンドシェイク
    SendSwitchUsbCommand(handle, 0x03u);   // 通信速度の切り替え
    SendSwitchUsbCommand(handle, 0x02u);   // 切り替え後のハンドシェイク
    SendSwitchUsbCommand(handle, 0x04u);   // HID のみのモード (タイムアウト無効)

    SendSwitchSubcommand(handle, packet_number, 0x40u, 0x01u);   // IMU を有効化
    SendSwitchSubcommand(handle, packet_number, 0x03u, 0x30u);   // 標準レポート (60Hz) へ
}

} // 無名名前空間


CHidGamepadSource::~CHidGamepadSource() noexcept {
    Shutdown();
}

void CHidGamepadSource::Shutdown() noexcept {
    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        FDevice& device = m_Devices[slot];
        if (device.handle != nullptr) {
            ::CancelIo(static_cast<HANDLE>(device.handle));
            ::CloseHandle(static_cast<HANDLE>(device.handle));
            device.handle = nullptr;
        }
        if (device.overlapped != nullptr) {
            OVERLAPPED* const overlapped = static_cast<OVERLAPPED*>(device.overlapped);
            if (overlapped->hEvent != nullptr) ::CloseHandle(overlapped->hEvent);
            delete overlapped;
            device.overlapped = nullptr;
        }
        device.kind = EHidGamepadKind::None;
        device.read_pending = false;
        m_States[slot] = FHidGamepadState{};
    }
}

const FHidGamepadState& CHidGamepadSource::GetState(u32 index) const noexcept {
    if (index >= kMaxDevices) return m_Empty;
    return m_States[index];
}

EHidGamepadKind CHidGamepadSource::GetKind(u32 index) const noexcept {
    if (index >= kMaxDevices) return EHidGamepadKind::None;
    return m_Devices[index].kind;
}

void CHidGamepadSource::Poll() noexcept {
    if (m_RescanCountdown == 0) {
        Rescan();
        m_RescanCountdown = kRescanInterval;
    } else {
        --m_RescanCountdown;
    }

    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        if (m_Devices[slot].handle == nullptr) continue;

        ReadDevice(slot);
    }
}

void CHidGamepadSource::Rescan() noexcept {
    GUID hid_guid {};
    ::HidD_GetHidGuid(&hid_guid);

    const HDEVINFO device_info = ::SetupDiGetClassDevsW(
        &hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_info == INVALID_HANDLE_VALUE) return;

    /** 今回の列挙で見つかったパスを控えて、消えたデバイスを閉じるのに使う。 */
    bool still_present[kMaxDevices] {};

    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0; ::SetupDiEnumDeviceInterfaces(device_info, nullptr, &hid_guid, index, &interface_data); ++index) {
        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(device_info, &interface_data, nullptr, 0, &required, nullptr);
        if (required == 0 || required > 1024) continue;

        /** デバイスパスを受け取る領域 (詳細構造体 + パス本体)。 */
        u8 detail_buffer[1024] {};
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* const detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!::SetupDiGetDeviceInterfaceDetailW(device_info, &interface_data, detail, required, nullptr, nullptr)) {
            continue;
        }

        // 既に開いているデバイスかを、パスの一致で判定する。
        char path_ascii[256] {};
        usize path_length = 0;
        for (; path_length + 1 < sizeof(path_ascii) && detail->DevicePath[path_length] != L'\0'; ++path_length) {
            path_ascii[path_length] = static_cast<char>(detail->DevicePath[path_length]);
        }
        path_ascii[path_length] = '\0';

        bool already_open = false;
        for (u32 slot = 0; slot < kMaxDevices; ++slot) {
            if (m_Devices[slot].handle == nullptr) continue;

            usize compare = 0;
            while (m_Devices[slot].path[compare] != '\0' && m_Devices[slot].path[compare] == path_ascii[compare]) {
                ++compare;
            }
            if (m_Devices[slot].path[compare] != '\0' || path_ascii[compare] != '\0') continue;

            still_present[slot] = true;
            already_open = true;
            break;
        }
        if (already_open) continue;

        // 空きスロットが無ければこれ以上は開かない。
        u32 free_slot = kMaxDevices;
        for (u32 slot = 0; slot < kMaxDevices; ++slot) {
            if (m_Devices[slot].handle != nullptr) continue;

            free_slot = slot;
            break;
        }
        if (free_slot == kMaxDevices) continue;

        // 読み書き両用で開く (Switch は初期化コマンドを送るため書き込みが要る)。
        HANDLE handle = ::CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes {};
        attributes.Size = sizeof(attributes);
        if (!::HidD_GetAttributes(handle, &attributes)) {
            ::CloseHandle(handle);
            continue;
        }

        const EHidGamepadKind kind = ClassifyDevice(attributes.VendorID, attributes.ProductID);
        if (kind == EHidGamepadKind::None) {
            ::CloseHandle(handle);
            continue;
        }

        // 入力レポート長を調べる (機種ごとに違う)。
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!::HidD_GetPreparsedData(handle, &preparsed)) {
            ::CloseHandle(handle);
            continue;
        }
        HIDP_CAPS caps {};
        const bool caps_ok = ::HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        ::HidD_FreePreparsedData(preparsed);
        if (!caps_ok || caps.InputReportByteLength == 0) {
            ::CloseHandle(handle);
            continue;
        }

        FDevice& device = m_Devices[free_slot];
        device.handle = handle;
        device.kind = kind;
        device.report_length = caps.InputReportByteLength;
        if (device.report_length > sizeof(device.report)) device.report_length = sizeof(device.report);
        device.read_pending = false;
        device.packet_number = 0;
        for (usize copy = 0; copy <= path_length; ++copy) device.path[copy] = path_ascii[copy];

        OVERLAPPED* const overlapped = new OVERLAPPED{};
        overlapped->hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        device.overlapped = overlapped;

        if (kind == EHidGamepadKind::SwitchPro || kind == EHidGamepadKind::JoyConLeft ||
            kind == EHidGamepadKind::JoyConRight) {
            InitializeSwitch(handle, device.packet_number);
        }

        m_States[free_slot] = FHidGamepadState{};
        m_States[free_slot].connected = true;
        still_present[free_slot] = true;
    }

    ::SetupDiDestroyDeviceInfoList(device_info);

    // 列挙に現れなかった = 抜かれたデバイスを閉じる。
    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        if (m_Devices[slot].handle == nullptr || still_present[slot]) continue;

        ::CancelIo(static_cast<HANDLE>(m_Devices[slot].handle));
        ::CloseHandle(static_cast<HANDLE>(m_Devices[slot].handle));
        m_Devices[slot].handle = nullptr;
        m_Devices[slot].kind = EHidGamepadKind::None;
        m_Devices[slot].read_pending = false;
        m_States[slot] = FHidGamepadState{};
    }
}

void CHidGamepadSource::ReadDevice(u32 slot) noexcept {
    FDevice& device = m_Devices[slot];
    HANDLE handle = static_cast<HANDLE>(device.handle);
    OVERLAPPED* const overlapped = static_cast<OVERLAPPED*>(device.overlapped);
    if (overlapped == nullptr || overlapped->hEvent == nullptr) return;

    // 読み取りを出していなければ 1 件発行する (完了はこのフレームか次以降)。
    if (!device.read_pending) {
        ::ResetEvent(overlapped->hEvent);
        DWORD read = 0;
        if (::ReadFile(handle, device.report, device.report_length, &read, overlapped)) {
            device.read_pending = true;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            device.read_pending = true;
        } else {
            // 抜かれた等で読めない。次の再列挙で拾い直す。
            return;
        }
    }

    // 完了していなければ待たずに抜ける (フレームを止めない)。
    if (::WaitForSingleObject(overlapped->hEvent, 0) != WAIT_OBJECT_0) return;

    DWORD transferred = 0;
    const bool ok = ::GetOverlappedResult(handle, overlapped, &transferred, FALSE) != FALSE;
    device.read_pending = false;
    if (!ok || transferred == 0) return;

    FHidGamepadState state {};
    state.connected = true;

    bool parsed = false;
    switch (device.kind) {
        case EHidGamepadKind::DualShock4:
            parsed = ParseDualShock4(device.report, static_cast<u32>(transferred), state);
            break;

        case EHidGamepadKind::DualSense:
            parsed = ParseDualSense(device.report, static_cast<u32>(transferred), state);
            break;

        case EHidGamepadKind::SwitchPro:
        case EHidGamepadKind::JoyConLeft:
        case EHidGamepadKind::JoyConRight:
            parsed = ParseSwitch(device.report, static_cast<u32>(transferred), device.kind, state);
            break;

        default:
            break;
    }

    // 解釈できないレポート (機器情報の応答など) は捨て、前の状態を保つ。
    if (parsed) m_States[slot] = state;
}

} // namespace acs
