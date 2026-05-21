// SPDX-License-Identifier: Apache-2.0
// 入力ポーリング実装
#include "platform/Input.h"
#include "foundation/Platform.h"

#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace acs {

namespace {

// 現フレーム / 前フレームの押下状態（フレーム遷移で「Pressed/Released」を判定）
struct InputState {
    bool keys_now [(usize)Key::_Count]         {};
    bool keys_prev[(usize)Key::_Count]         {};
    bool mb_now [(usize)MouseButton::_Count]   {};
    bool mb_prev[(usize)MouseButton::_Count]   {};
    f32  mouse_x = 0, mouse_y = 0;
    f32  mouse_x_prev = 0, mouse_y_prev = 0;
    f32  wheel_accum = 0;     // 当該フレーム中に積み上げ
    f32  wheel_frame = 0;     // Update 時にスナップショット

    // テキスト入力（このフレームに積み上げた UTF-8 文字列、NUL 終端）
    char text_utf8[256] {};
    u32  text_len     = 0;
    u32  hi_surrogate = 0;    // UTF-16 上位サロゲート待ち

    // ゲームパッド (XInput) — 4 人分
    XINPUT_STATE pad_now [4] {};
    XINPUT_STATE pad_prev[4] {};
    bool         pad_connected[4] {};
};

InputState g_input;

// XInput ボタンビット → GamepadButton 変換テーブル
constexpr WORD kPadBits[(usize)GamepadButton::_Count] = {
    XINPUT_GAMEPAD_A,              // A
    XINPUT_GAMEPAD_B,              // B
    XINPUT_GAMEPAD_X,              // X
    XINPUT_GAMEPAD_Y,              // Y
    XINPUT_GAMEPAD_DPAD_UP,        // Up
    XINPUT_GAMEPAD_DPAD_DOWN,      // Down
    XINPUT_GAMEPAD_DPAD_LEFT,      // Left
    XINPUT_GAMEPAD_DPAD_RIGHT,     // Right
    XINPUT_GAMEPAD_LEFT_SHOULDER,  // LeftBumper
    XINPUT_GAMEPAD_RIGHT_SHOULDER, // RightBumper
    XINPUT_GAMEPAD_LEFT_THUMB,     // LeftStick
    XINPUT_GAMEPAD_RIGHT_THUMB,    // RightStick
    XINPUT_GAMEPAD_START,          // Start
    XINPUT_GAMEPAD_BACK,           // Back
    0,                              // Guide (XInput では未公開)
};

// スティック生値 → -1.0〜+1.0 正規化（デッドゾーン込み）
ACS_FORCEINLINE f32 NormalizeStick(SHORT v, SHORT deadzone) noexcept {
    if (v > -deadzone && v < deadzone) return 0.0f;
    f32 n = static_cast<f32>(v);
    f32 max_v = (v > 0) ? 32767.0f : 32768.0f;
    return n / max_v;
}

// トリガー生値 → 0.0〜1.0
ACS_FORCEINLINE f32 NormalizeTrigger(BYTE v) noexcept {
    if (v < XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return 0.0f;
    return static_cast<f32>(v) / 255.0f;
}

// コードポイントを UTF-8 で text_utf8 末尾に追記する
void AppendTextUtf8(InputState& s, u32 cp) noexcept {
    char tmp[4];
    u32  n = 0;
    if (cp < 0x80) {
        tmp[0] = static_cast<char>(cp); n = 1;
    } else if (cp < 0x800) {
        tmp[0] = static_cast<char>(0xC0 | (cp >> 6));
        tmp[1] = static_cast<char>(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = static_cast<char>(0xE0 | (cp >> 12));
        tmp[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = static_cast<char>(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = static_cast<char>(0xF0 | (cp >> 18));
        tmp[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = static_cast<char>(0x80 | (cp & 0x3F)); n = 4;
    }
    if (s.text_len + n + 1 > sizeof(s.text_utf8)) return;   // バッファ満杯
    for (u32 i = 0; i < n; ++i) s.text_utf8[s.text_len++] = tmp[i];
    s.text_utf8[s.text_len] = 0;
}

} // namespace

// フレーム先頭で呼ぶ：状態をフレーム間で進める
void Input::Update() noexcept {
    // キーボード / マウス: 現フレーム → 前フレームに移し替え
    for (usize i = 0; i < (usize)Key::_Count; ++i)
        g_input.keys_prev[i] = g_input.keys_now[i];
    for (usize i = 0; i < (usize)MouseButton::_Count; ++i)
        g_input.mb_prev[i] = g_input.mb_now[i];

    // マウス位置の差分を確定
    g_input.mouse_x_prev = g_input.mouse_x;
    g_input.mouse_y_prev = g_input.mouse_y;

    // ホイール: フレーム中に積み上げた値をスナップショット → 累積をリセット
    g_input.wheel_frame = g_input.wheel_accum;
    g_input.wheel_accum = 0.0f;

    // テキスト入力: 前フレームの入力をクリア
    g_input.text_len     = 0;
    g_input.text_utf8[0] = 0;

    // ゲームパッド: XInput を 4 ポート分ポーリング
    for (DWORD i = 0; i < 4; ++i) {
        g_input.pad_prev[i] = g_input.pad_now[i];
        ZeroMemory(&g_input.pad_now[i], sizeof(XINPUT_STATE));
        DWORD r = ::XInputGetState(i, &g_input.pad_now[i]);
        g_input.pad_connected[i] = (r == ERROR_SUCCESS);
    }
}

// Window からの Event を Input 状態に反映
void Input::OnEvent(const Event& e) noexcept {
    switch (e.type) {
        case EventType::KeyPressed:
        case EventType::KeyRepeat:
            g_input.keys_now[(usize)e.key.key] = true;
            break;
        case EventType::KeyReleased:
            g_input.keys_now[(usize)e.key.key] = false;
            break;
        case EventType::MouseButtonPressed:
            g_input.mb_now[(usize)e.mouse_button.button] = true;
            break;
        case EventType::MouseButtonReleased:
            g_input.mb_now[(usize)e.mouse_button.button] = false;
            break;
        case EventType::MouseMoved:
            g_input.mouse_x = e.mouse_move.x;
            g_input.mouse_y = e.mouse_move.y;
            break;
        case EventType::MouseScrolled:
            g_input.wheel_accum += e.mouse_scroll.y;
            break;
        case EventType::CharInput: {
            u32 cp = e.char_input.codepoint;
            if (cp >= 0xD800 && cp <= 0xDBFF) {            // UTF-16 上位サロゲート
                g_input.hi_surrogate = cp;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {     // 下位サロゲート
                if (g_input.hi_surrogate) {
                    cp = 0x10000u + ((g_input.hi_surrogate - 0xD800u) << 10)
                                  + (cp - 0xDC00u);
                    g_input.hi_surrogate = 0;
                    AppendTextUtf8(g_input, cp);
                }
            } else {
                g_input.hi_surrogate = 0;
                AppendTextUtf8(g_input, cp);
            }
            break;
        }
        default: break;
    }
}

bool Input::IsKeyDown(Key k) noexcept     { return g_input.keys_now[(usize)k]; }
bool Input::IsKeyPressed(Key k) noexcept  { return g_input.keys_now[(usize)k] && !g_input.keys_prev[(usize)k]; }
bool Input::IsKeyReleased(Key k) noexcept { return !g_input.keys_now[(usize)k] && g_input.keys_prev[(usize)k]; }

bool Input::IsMouseButtonDown(MouseButton b) noexcept     { return g_input.mb_now[(usize)b]; }
bool Input::IsMouseButtonPressed(MouseButton b) noexcept  { return g_input.mb_now[(usize)b] && !g_input.mb_prev[(usize)b]; }
bool Input::IsMouseButtonReleased(MouseButton b) noexcept { return !g_input.mb_now[(usize)b] && g_input.mb_prev[(usize)b]; }

Vec2 Input::MousePos()   noexcept { return Vec2(g_input.mouse_x, g_input.mouse_y); }
Vec2 Input::MouseDelta() noexcept { return Vec2(g_input.mouse_x - g_input.mouse_x_prev,
                                                 g_input.mouse_y - g_input.mouse_y_prev); }
f32  Input::MouseWheel() noexcept { return g_input.wheel_frame; }

const char* Input::TextInput() noexcept { return g_input.text_utf8; }

bool Input::IsGamepadConnected(u32 idx) noexcept {
    if (idx >= 4) return false;
    return g_input.pad_connected[idx];
}

bool Input::IsGamepadButtonDown(u32 idx, GamepadButton b) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return false;
    WORD bit = kPadBits[(usize)b];
    if (bit == 0) return false;
    return (g_input.pad_now[idx].Gamepad.wButtons & bit) != 0;
}

bool Input::IsGamepadButtonPressed(u32 idx, GamepadButton b) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return false;
    WORD bit = kPadBits[(usize)b];
    if (bit == 0) return false;
    bool now  = (g_input.pad_now[idx].Gamepad.wButtons  & bit) != 0;
    bool prev = (g_input.pad_prev[idx].Gamepad.wButtons & bit) != 0;
    return now && !prev;
}

f32 Input::GamepadAxisValue(u32 idx, GamepadAxis axis) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return 0.0f;
    const auto& g = g_input.pad_now[idx].Gamepad;
    switch (axis) {
        case GamepadAxis::LeftX:        return NormalizeStick(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        case GamepadAxis::LeftY:        return NormalizeStick(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        case GamepadAxis::RightX:       return NormalizeStick(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        case GamepadAxis::RightY:       return NormalizeStick(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        case GamepadAxis::LeftTrigger:  return NormalizeTrigger(g.bLeftTrigger);
        case GamepadAxis::RightTrigger: return NormalizeTrigger(g.bRightTrigger);
        default: return 0.0f;
    }
}

} // namespace acs
