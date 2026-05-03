// キーボード / マウスボタン / ゲームパッドボタンの定数
#pragma once

#include "foundation/Types.h"

namespace acs {

// キーボードキー（Virtual Key Code に近い列挙、初学者にも分かりやすい名前）
enum class Key : u16 {
    Unknown = 0,

    // 文字キー
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // 数字キー (上段)
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

    // ファンクションキー
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // 修飾キー
    LeftShift, RightShift,
    LeftCtrl,  RightCtrl,
    LeftAlt,   RightAlt,
    LeftSuper, RightSuper,  // Windows キー / Cmd キー

    // 矢印キー
    Up, Down, Left, Right,

    // 制御キー
    Space, Enter, Tab, Backspace, Escape,
    Insert, Delete, Home, End, PageUp, PageDown,
    CapsLock, NumLock, ScrollLock,

    // 記号キー
    Minus, Equal, LeftBracket, RightBracket,
    Backslash, Semicolon, Apostrophe, Comma, Period, Slash, Grave,

    // テンキー
    KP0, KP1, KP2, KP3, KP4, KP5, KP6, KP7, KP8, KP9,
    KPAdd, KPSubtract, KPMultiply, KPDivide, KPEnter, KPDecimal,

    _Count
};

// マウスボタン
enum class MouseButton : u8 {
    Left   = 0,
    Right  = 1,
    Middle = 2,
    X1     = 3,   // サイドボタン 1
    X2     = 4,   // サイドボタン 2
    _Count
};

// ゲームパッドボタン (XInput 互換)
enum class GamepadButton : u8 {
    A,        B,        X,        Y,
    Up,       Down,     Left,     Right,    // D-Pad
    LeftBumper,         RightBumper,
    LeftStick,          RightStick,         // スティック押し込み
    Start,    Back,     Guide,
    _Count
};

// ゲームパッドの軸 (-1.0 〜 +1.0、トリガーは 0.0 〜 1.0)
enum class GamepadAxis : u8 {
    LeftX,    LeftY,
    RightX,   RightY,
    LeftTrigger,  RightTrigger,
    _Count
};

} // namespace acs
