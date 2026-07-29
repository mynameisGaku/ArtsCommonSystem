// SPDX-License-Identifier: Apache-2.0
// 入力ポーリング実装
#include "platform/Input.h"
#include "foundation/Platform.h"

#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace acs {

namespace {

/**
 * 全入力デバイスの現フレーム / 前フレーム状態をまとめた構造体。
 *
 * @details now と prev を比較してフレーム単位の「Pressed / Released」を判定する。
 */
struct FInputState {
    /** 現フレームのキー押下状態 (EKey で添字)。 */
    bool keys_now [(usize)EKey::_Count]         {};

    /** 前フレームのキー押下状態 (EKey で添字)。 */
    bool keys_prev[(usize)EKey::_Count]         {};

    /** 現フレームのマウスボタン押下状態 (EMouseButton で添字)。 */
    bool mb_now [(usize)EMouseButton::_Count]   {};

    /** 前フレームのマウスボタン押下状態 (EMouseButton で添字)。 */
    bool mb_prev[(usize)EMouseButton::_Count]   {};

    /** 現在のマウス X 座標 (クライアント座標, px)。 */
    f32  mouse_x = 0;

    /** 現在のマウス Y 座標 (クライアント座標, px)。 */
    f32  mouse_y = 0;

    /** 前フレームのマウス X 座標 (差分計算用)。 */
    f32  mouse_x_prev = 0;

    /** 前フレームのマウス Y 座標 (差分計算用)。 */
    f32  mouse_y_prev = 0;

    /** 当該フレーム中に積み上げるホイール回転量。 */
    f32  wheel_accum = 0;

    /** Update 時にスナップショットしたホイール回転量 (MouseWheel が返す値)。 */
    f32  wheel_frame = 0;

    /** このフレームに積み上げたテキスト入力 (UTF-8、NUL 終端)。 */
    char text_utf8[256] {};

    /** text_utf8 に積まれた現在のバイト長。 */
    u32  text_len     = 0;

    /** 待機中の UTF-16 上位サロゲート (無ければ 0)。 */
    u32  hi_surrogate = 0;

    /** 現フレームの XInput 状態 (プレイヤー 0..3)。 */
    XINPUT_STATE pad_now [4] {};

    /** 前フレームの XInput 状態 (プレイヤー 0..3、Pressed 判定用)。 */
    XINPUT_STATE pad_prev[4] {};

    /** 各プレイヤーのゲームパッド接続状態。 */
    bool         pad_connected[4] {};
};

/** プロセス唯一の入力状態 (全 static メソッドが参照する)。 */
FInputState g_input;

/** 接続中は毎フレーム取得し、未接続確認だけをフレーム間へ分散する。 */
detail::TGamepadPollScheduler<4> g_gamepad_poll_scheduler;

/** EGamepadButton から XInput のボタンビットへ変換するテーブル (0 は未対応ボタン)。 */
constexpr WORD kPadBits[(usize)EGamepadButton::_Count] = {
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

/**
 * スティックの生値をデッドゾーン込みで -1.0〜+1.0 に正規化する。
 *
 * @param v スティックの生値 (-32768〜32767)。
 * @param deadzone この絶対値未満は 0 に丸めるデッドゾーン閾値。
 * @return 正規化した値 (デッドゾーン内なら 0.0)。
 */
ACS_FORCEINLINE f32 NormalizeStick(SHORT v, SHORT deadzone) noexcept {
    if (v > -deadzone && v < deadzone) return 0.0f;
    const f32 n = static_cast<f32>(v);
    const f32 max_v = (v > 0) ? 32767.0f : 32768.0f;
    return n / max_v;
}

/**
 * トリガーの生値を 0.0〜1.0 に正規化する。
 *
 * @param v トリガーの生値 (0〜255)。
 * @return 正規化した値 (XInput の閾値未満なら 0.0)。
 */
ACS_FORCEINLINE f32 NormalizeTrigger(BYTE v) noexcept {
    if (v < XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return 0.0f;
    return static_cast<f32>(v) / 255.0f;
}

/**
 * Unicode コードポイントを UTF-8 エンコードして text_utf8 の末尾へ追記する。
 *
 * @details NUL 終端ぶんを含めてバッファに収まらない場合は何もしない。
 * @param s 追記先の入力状態。
 * @param cp 追記する Unicode コードポイント。
 */
void AppendTextUtf8(FInputState& s, u32 cp) noexcept {
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
void FInput::Update() noexcept {
    // キーボード / マウス: 現フレーム → 前フレームに移し替え
    for (usize i = 0; i < (usize)EKey::_Count; ++i)
        g_input.keys_prev[i] = g_input.keys_now[i];
    for (usize i = 0; i < (usize)EMouseButton::_Count; ++i)
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
    // 先に全スナップショットを進める。次フレームに未接続ポートを確認しない場合でも、
    // 切断による Released はちょうど1フレームだけ成立する。
    for (DWORD i = 0; i < 4; ++i) {
        g_input.pad_prev[i] = g_input.pad_now[i];
    }

    // 接続中デバイスは毎フレーム取得し、失敗する未接続確認だけを分散する。
    const u32 poll_mask = g_gamepad_poll_scheduler.BuildPollMask(g_input.pad_connected);
    for (DWORD i = 0; i < 4; ++i) {
        if ((poll_mask & (u32{1} << i)) == 0) continue;
        ZeroMemory(&g_input.pad_now[i], sizeof(XINPUT_STATE));
        const DWORD r = ::XInputGetState(i, &g_input.pad_now[i]);
        g_input.pad_connected[i] = (r == ERROR_SUCCESS);
    }
}

// FWindow からの Event を Input 状態に反映
void FInput::OnEvent(const FEvent& e) noexcept {
    switch (e.type) {
        case EEventType::KeyPressed:
        case EEventType::KeyRepeat:
            g_input.keys_now[(usize)e.key.key] = true;
            break;
        case EEventType::KeyReleased:
            g_input.keys_now[(usize)e.key.key] = false;
            break;
        case EEventType::MouseButtonPressed:
            g_input.mb_now[(usize)e.mouse_button.button] = true;
            break;
        case EEventType::MouseButtonReleased:
            g_input.mb_now[(usize)e.mouse_button.button] = false;
            break;
        case EEventType::MouseMoved:
            g_input.mouse_x = e.mouse_move.x;
            g_input.mouse_y = e.mouse_move.y;
            break;
        case EEventType::MouseScrolled:
            g_input.wheel_accum += e.mouse_scroll.y;
            break;
        case EEventType::CharInput: {
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

bool FInput::IsKeyDown(EKey k) noexcept     { return g_input.keys_now[(usize)k]; }
bool FInput::IsKeyPressed(EKey k) noexcept  { return g_input.keys_now[(usize)k] && !g_input.keys_prev[(usize)k]; }
bool FInput::IsKeyReleased(EKey k) noexcept { return !g_input.keys_now[(usize)k] && g_input.keys_prev[(usize)k]; }

bool FInput::IsMouseButtonDown(EMouseButton b) noexcept     { return g_input.mb_now[(usize)b]; }
bool FInput::IsMouseButtonPressed(EMouseButton b) noexcept  { return g_input.mb_now[(usize)b] && !g_input.mb_prev[(usize)b]; }
bool FInput::IsMouseButtonReleased(EMouseButton b) noexcept { return !g_input.mb_now[(usize)b] && g_input.mb_prev[(usize)b]; }

FVec2 FInput::MousePos()   noexcept { return FVec2(g_input.mouse_x, g_input.mouse_y); }
FVec2 FInput::MouseDelta() noexcept { return FVec2(g_input.mouse_x - g_input.mouse_x_prev,
                                                 g_input.mouse_y - g_input.mouse_y_prev); }
f32  FInput::MouseWheel() noexcept { return g_input.wheel_frame; }

const char* FInput::TextInput() noexcept { return g_input.text_utf8; }

bool FInput::IsGamepadConnected(u32 idx) noexcept {
    if (idx >= 4) return false;
    return g_input.pad_connected[idx];
}

bool FInput::IsGamepadButtonDown(u32 idx, EGamepadButton b) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return false;
    const usize button_index = static_cast<usize>(b);
    if (button_index >= static_cast<usize>(EGamepadButton::_Count)) return false;
    const WORD bit = kPadBits[button_index];
    if (bit == 0) return false;
    return (g_input.pad_now[idx].Gamepad.wButtons & bit) != 0;
}

bool FInput::IsGamepadButtonPressed(u32 idx, EGamepadButton b) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return false;
    const usize button_index = static_cast<usize>(b);
    if (button_index >= static_cast<usize>(EGamepadButton::_Count)) return false;
    const WORD bit = kPadBits[button_index];
    if (bit == 0) return false;
    const bool now  = (g_input.pad_now[idx].Gamepad.wButtons  & bit) != 0;
    const bool prev = (g_input.pad_prev[idx].Gamepad.wButtons & bit) != 0;
    return now && !prev;
}

bool FInput::IsGamepadButtonReleased(u32 idx, EGamepadButton b) noexcept
{
    if (idx >= 4) return false;
    const usize button_index = static_cast<usize>(b);
    if (button_index >= static_cast<usize>(EGamepadButton::_Count)) return false;
    const WORD bit = kPadBits[button_index];
    if (bit == 0) return false;
    const bool now = (g_input.pad_now[idx].Gamepad.wButtons & bit) != 0;
    const bool prev = (g_input.pad_prev[idx].Gamepad.wButtons & bit) != 0;
    return !now && prev;
}

f32 FInput::GamepadAxisValue(u32 idx, EGamepadAxis axis) noexcept {
    if (idx >= 4 || !g_input.pad_connected[idx]) return 0.0f;
    const auto& g = g_input.pad_now[idx].Gamepad;
    switch (axis) {
        case EGamepadAxis::LeftX:        return NormalizeStick(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        case EGamepadAxis::LeftY:        return NormalizeStick(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        case EGamepadAxis::RightX:       return NormalizeStick(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        case EGamepadAxis::RightY:       return NormalizeStick(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        case EGamepadAxis::LeftTrigger:  return NormalizeTrigger(g.bLeftTrigger);
        case EGamepadAxis::RightTrigger: return NormalizeTrigger(g.bRightTrigger);
        default: return 0.0f;
    }
}

} // namespace acs
