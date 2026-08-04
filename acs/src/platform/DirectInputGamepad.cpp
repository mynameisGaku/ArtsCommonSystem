// SPDX-License-Identifier: Apache-2.0
// DirectInput ゲームパッド読み取り実装 (XInput / HID 機種別対応から漏れる汎用パッド用)

#include "platform/DirectInputGamepad.h"

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace acs {

namespace {

/** 再列挙するまでの Poll 回数 (毎フレーム列挙すると重いので間引く)。 */
constexpr u32 kRescanInterval = 240;

/** スティックの中央付近を切り捨てる幅 (0..1)。 */
constexpr f32 kStickDeadzone = 0.15f;

/** DirectInput の軸の範囲 (SetProperty で ±1000 に正規化する)。 */
constexpr LONG kAxisRange = 1000;

/** 拾うボタンの上限 (DirectInput は 128 個まで持てるが、パッドとして意味があるのは先頭のみ)。 */
constexpr u32 kMaxButtons = 16;

/**
 * 列挙中に見つけたデバイスを受け取るための作業領域。
 */
struct FEnumContext {
    /** 生成に使う IDirectInput8W。 */
    IDirectInput8W* direct_input = nullptr;

    /** 見つけた機器の GUID を積む先。 */
    GUID found[CDirectInputGamepadSource::kMaxDevices] {};

    /** 積んだ数。 */
    u32 found_count = 0;
};

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
 * ±1000 の軸生値を -1..+1 へ正規化する。
 *
 * @param raw 生値。
 * @return デッドゾーン処理済みの値。
 */
f32 NormalizeAxis(LONG raw) noexcept {
    /** -1..+1 へ写した値。 */
    f32 value = static_cast<f32>(raw) / static_cast<f32>(kAxisRange);
    if (value > -kStickDeadzone && value < kStickDeadzone) return 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return value;
}

/**
 * POV (十字キー) の値を方向ボタンへ展開する。
 *
 * @param buttons 立てる先。
 * @param pov 100 分の 1 度単位の角度 (中立は 0xFFFF)。
 */
void ApplyPov(u32& buttons, DWORD pov) noexcept {
    if (LOWORD(pov) == 0xFFFFu) return;

    /** 度に直した角度。 */
    const u32 degrees = static_cast<u32>(pov / 100u) % 360u;
    SetButton(buttons, EGamepadButton::Up,    degrees > 292u || degrees < 68u);
    SetButton(buttons, EGamepadButton::Right, degrees > 22u  && degrees < 158u);
    SetButton(buttons, EGamepadButton::Down,  degrees > 112u && degrees < 248u);
    SetButton(buttons, EGamepadButton::Left,  degrees > 202u && degrees < 338u);
}

/**
 * XInput からも見えている機器かを調べる。
 *
 * @details
 * XInput 対応パッドは DirectInput にも現れるため、そのままだと二重に数えてしまう。
 * XInput 側の機器は識別子に "IG_" を含むという既知の目印で見分ける。
 * @param direct_input 生成に使う IDirectInput8W。
 * @param instance 判定する機器の GUID。
 * @return XInput 側でも見えているなら true。
 */
bool IsXInputDevice(IDirectInput8W* direct_input, const GUID& instance) noexcept {
    IDirectInputDevice8W* device = nullptr;
    if (FAILED(direct_input->CreateDevice(instance, &device, nullptr)) || device == nullptr) {
        return false;
    }

    DIPROPGUIDANDPATH property {};
    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;

    bool is_xinput = false;
    if (SUCCEEDED(device->GetProperty(DIPROP_GUIDANDPATH, &property.diph))) {
        for (u32 index = 0; index + 2 < sizeof(property.wszPath) / sizeof(property.wszPath[0]); ++index) {
            if (property.wszPath[index] == L'\0') break;
            if (property.wszPath[index] == L'i' && property.wszPath[index + 1] == L'g' &&
                property.wszPath[index + 2] == L'_') {
                is_xinput = true;
                break;
            }
            if (property.wszPath[index] == L'I' && property.wszPath[index + 1] == L'G' &&
                property.wszPath[index + 2] == L'_') {
                is_xinput = true;
                break;
            }
        }
    }

    device->Release();
    return is_xinput;
}

/**
 * デバイス列挙の callback。
 *
 * @param instance 見つかった機器。
 * @param context FEnumContext。
 * @return 続行なら DIENUM_CONTINUE。
 */
BOOL CALLBACK EnumDeviceCallback(const DIDEVICEINSTANCEW* instance, void* context) noexcept {
    FEnumContext* const enum_context = static_cast<FEnumContext*>(context);
    if (enum_context == nullptr || instance == nullptr) return DIENUM_STOP;
    if (enum_context->found_count >= CDirectInputGamepadSource::kMaxDevices) return DIENUM_STOP;

    // XInput 側で見えているパッドはそちらに任せる。
    if (IsXInputDevice(enum_context->direct_input, instance->guidInstance)) return DIENUM_CONTINUE;

    enum_context->found[enum_context->found_count] = instance->guidInstance;
    ++enum_context->found_count;
    return DIENUM_CONTINUE;
}

/**
 * 軸の範囲を ±1000 に揃える callback。
 *
 * @param object 対象の軸。
 * @param context IDirectInputDevice8W。
 * @return 続行なら DIENUM_CONTINUE。
 */
BOOL CALLBACK EnumAxisCallback(const DIDEVICEOBJECTINSTANCEW* object, void* context) noexcept {
    IDirectInputDevice8W* const device = static_cast<IDirectInputDevice8W*>(context);
    if (device == nullptr || object == nullptr) return DIENUM_STOP;

    DIPROPRANGE range {};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = object->dwType;
    range.lMin = -kAxisRange;
    range.lMax = kAxisRange;
    device->SetProperty(DIPROP_RANGE, &range.diph);
    return DIENUM_CONTINUE;
}

} // 無名名前空間


CDirectInputGamepadSource::~CDirectInputGamepadSource() noexcept {
    Shutdown();
}

void CDirectInputGamepadSource::Shutdown() noexcept {
    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        if (m_Devices[slot].device != nullptr) {
            IDirectInputDevice8W* const device = static_cast<IDirectInputDevice8W*>(m_Devices[slot].device);
            device->Unacquire();
            device->Release();
            m_Devices[slot].device = nullptr;
        }
        m_States[slot] = FDirectInputGamepadState{};
    }

    if (m_DirectInput != nullptr) {
        static_cast<IDirectInput8W*>(m_DirectInput)->Release();
        m_DirectInput = nullptr;
    }
}

const FDirectInputGamepadState& CDirectInputGamepadSource::GetState(u32 index) const noexcept {
    if (index >= kMaxDevices) return m_Empty;
    return m_States[index];
}

void CDirectInputGamepadSource::Poll() noexcept {
    if (!m_Tried) {
        m_Tried = true;
        IDirectInput8W* direct_input = nullptr;
        const HRESULT result = ::DirectInput8Create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                                    IID_IDirectInput8W,
                                                    reinterpret_cast<void**>(&direct_input), nullptr);
        m_DirectInput = SUCCEEDED(result) ? direct_input : nullptr;
    }
    if (m_DirectInput == nullptr) return;

    if (m_RescanCountdown == 0) {
        Rescan();
        m_RescanCountdown = kRescanInterval;
    } else {
        --m_RescanCountdown;
    }

    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        if (m_Devices[slot].device == nullptr) continue;

        IDirectInputDevice8W* const device = static_cast<IDirectInputDevice8W*>(m_Devices[slot].device);

        // 取得できない間は取り直しを試みる (フォーカスを失った後など)。
        if (FAILED(device->Poll())) {
            if (FAILED(device->Acquire())) {
                m_States[slot] = FDirectInputGamepadState{};
                continue;
            }
            device->Poll();
        }

        DIJOYSTATE2 raw {};
        if (FAILED(device->GetDeviceState(sizeof(raw), &raw))) {
            m_States[slot] = FDirectInputGamepadState{};
            continue;
        }

        FDirectInputGamepadState state {};
        state.connected = true;

        u32 buttons = 0;
        for (u32 index = 0; index < kMaxButtons; ++index) {
            if ((raw.rgbButtons[index] & 0x80u) == 0) continue;

            // 並びは機器任せなので、DirectInput の慣習どおり 0 番から順に当てる。
            switch (index) {
                case 0:  SetButton(buttons, EGamepadButton::South, true); break;
                case 1:  SetButton(buttons, EGamepadButton::East, true); break;
                case 2:  SetButton(buttons, EGamepadButton::West, true); break;
                case 3:  SetButton(buttons, EGamepadButton::North, true); break;
                case 4:  SetButton(buttons, EGamepadButton::LeftBumper, true); break;
                case 5:  SetButton(buttons, EGamepadButton::RightBumper, true); break;
                case 6:  SetButton(buttons, EGamepadButton::Back, true); break;
                case 7:  SetButton(buttons, EGamepadButton::Start, true); break;
                case 8:  SetButton(buttons, EGamepadButton::LeftStick, true); break;
                case 9:  SetButton(buttons, EGamepadButton::RightStick, true); break;
                case 10: SetButton(buttons, EGamepadButton::Guide, true); break;
                default: break;
            }
        }
        ApplyPov(buttons, raw.rgdwPOV[0]);
        state.buttons = buttons;

        state.axes[static_cast<usize>(EGamepadAxis::LeftX)]  = NormalizeAxis(raw.lX);
        state.axes[static_cast<usize>(EGamepadAxis::LeftY)]  = -NormalizeAxis(raw.lY);
        state.axes[static_cast<usize>(EGamepadAxis::RightX)] = NormalizeAxis(raw.lZ);
        state.axes[static_cast<usize>(EGamepadAxis::RightY)] = -NormalizeAxis(raw.lRz);

        // アナログトリガーの割り当ては機器ごとに違い (lRx/lRy だったり lZ 1 本に合成されたり)、
        // DirectInput からは見分けが付かないので出さない。押した / 離したはボタン側で拾える。
        state.axes[static_cast<usize>(EGamepadAxis::LeftTrigger)] = 0.0f;
        state.axes[static_cast<usize>(EGamepadAxis::RightTrigger)] = 0.0f;

        m_States[slot] = state;
    }
}

void CDirectInputGamepadSource::Rescan() noexcept {
    IDirectInput8W* const direct_input = static_cast<IDirectInput8W*>(m_DirectInput);
    if (direct_input == nullptr) return;

    FEnumContext context {};
    context.direct_input = direct_input;
    direct_input->EnumDevices(DI8DEVCLASS_GAMECTRL, &EnumDeviceCallback, &context, DIEDFL_ATTACHEDONLY);

    /** 今回の列挙にも現れたスロット。 */
    bool still_present[kMaxDevices] {};

    for (u32 found_index = 0; found_index < context.found_count; ++found_index) {
        const GUID& instance = context.found[found_index];

        // 既に開いているかを GUID の一致で調べる。
        bool already_open = false;
        for (u32 slot = 0; slot < kMaxDevices; ++slot) {
            if (m_Devices[slot].device == nullptr) continue;

            bool same = true;
            const u8* const bytes = reinterpret_cast<const u8*>(&instance);
            for (u32 byte_index = 0; byte_index < sizeof(m_Devices[slot].instance_guid); ++byte_index) {
                if (m_Devices[slot].instance_guid[byte_index] == bytes[byte_index]) continue;

                same = false;
                break;
            }
            if (!same) continue;

            still_present[slot] = true;
            already_open = true;
            break;
        }
        if (already_open) continue;

        /** 空きスロット。 */
        u32 free_slot = kMaxDevices;
        for (u32 slot = 0; slot < kMaxDevices; ++slot) {
            if (m_Devices[slot].device != nullptr) continue;

            free_slot = slot;
            break;
        }
        if (free_slot == kMaxDevices) continue;

        IDirectInputDevice8W* device = nullptr;
        if (FAILED(direct_input->CreateDevice(instance, &device, nullptr)) || device == nullptr) continue;

        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) {
            device->Release();
            continue;
        }

        // ウィンドウを持たない層から使うため、前面でなくても読める設定にする。
        device->SetCooperativeLevel(::GetDesktopWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        device->EnumObjects(&EnumAxisCallback, device, DIDFT_AXIS);
        device->Acquire();

        m_Devices[free_slot].device = device;
        const u8* const bytes = reinterpret_cast<const u8*>(&instance);
        for (u32 byte_index = 0; byte_index < sizeof(m_Devices[free_slot].instance_guid); ++byte_index) {
            m_Devices[free_slot].instance_guid[byte_index] = bytes[byte_index];
        }
        m_States[free_slot] = FDirectInputGamepadState{};
        m_States[free_slot].connected = true;
        still_present[free_slot] = true;
    }

    // 列挙に現れなかった = 抜かれたデバイスを閉じる。
    for (u32 slot = 0; slot < kMaxDevices; ++slot) {
        if (m_Devices[slot].device == nullptr || still_present[slot]) continue;

        IDirectInputDevice8W* const device = static_cast<IDirectInputDevice8W*>(m_Devices[slot].device);
        device->Unacquire();
        device->Release();
        m_Devices[slot].device = nullptr;
        m_States[slot] = FDirectInputGamepadState{};
    }
}

} // namespace acs
