// SPDX-License-Identifier: Apache-2.0
// キーボード / マウスボタン / ゲームパッドボタンの定数
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * キーボードのキー識別子。
 *
 * @details
 * Win32 仮想キーコードに対応するが、初学者にも分かりやすい名前を採用している。
 * 値は配列添字に使うため連続。_Count は要素数 (番兵) で、実キーではない。
 */
enum class EKey : u16 {
    /** 未対応・未知のキー (変換できなかった仮想キー)。 */
    Unknown = 0,

    /** 文字キー A。 */
    A,
    /** 文字キー B。 */
    B,
    /** 文字キー C。 */
    C,
    /** 文字キー D。 */
    D,
    /** 文字キー E。 */
    E,
    /** 文字キー F。 */
    F,
    /** 文字キー G。 */
    G,
    /** 文字キー H。 */
    H,
    /** 文字キー I。 */
    I,
    /** 文字キー J。 */
    J,
    /** 文字キー K。 */
    K,
    /** 文字キー L。 */
    L,
    /** 文字キー M。 */
    M,
    /** 文字キー N。 */
    N,
    /** 文字キー O。 */
    O,
    /** 文字キー P。 */
    P,
    /** 文字キー Q。 */
    Q,
    /** 文字キー R。 */
    R,
    /** 文字キー S。 */
    S,
    /** 文字キー T。 */
    T,
    /** 文字キー U。 */
    U,
    /** 文字キー V。 */
    V,
    /** 文字キー W。 */
    W,
    /** 文字キー X。 */
    X,
    /** 文字キー Y。 */
    Y,
    /** 文字キー Z。 */
    Z,

    /** 上段の数字キー 0。 */
    Num0,
    /** 上段の数字キー 1。 */
    Num1,
    /** 上段の数字キー 2。 */
    Num2,
    /** 上段の数字キー 3。 */
    Num3,
    /** 上段の数字キー 4。 */
    Num4,
    /** 上段の数字キー 5。 */
    Num5,
    /** 上段の数字キー 6。 */
    Num6,
    /** 上段の数字キー 7。 */
    Num7,
    /** 上段の数字キー 8。 */
    Num8,
    /** 上段の数字キー 9。 */
    Num9,

    /** ファンクションキー F1。 */
    F1,
    /** ファンクションキー F2。 */
    F2,
    /** ファンクションキー F3。 */
    F3,
    /** ファンクションキー F4。 */
    F4,
    /** ファンクションキー F5。 */
    F5,
    /** ファンクションキー F6。 */
    F6,
    /** ファンクションキー F7。 */
    F7,
    /** ファンクションキー F8。 */
    F8,
    /** ファンクションキー F9。 */
    F9,
    /** ファンクションキー F10。 */
    F10,
    /** ファンクションキー F11。 */
    F11,
    /** ファンクションキー F12。 */
    F12,

    /** 左 Shift キー。 */
    LeftShift,
    /** 右 Shift キー。 */
    RightShift,
    /** 左 Ctrl キー。 */
    LeftCtrl,
    /** 右 Ctrl キー。 */
    RightCtrl,
    /** 左 Alt キー。 */
    LeftAlt,
    /** 右 Alt キー。 */
    RightAlt,
    /** 左 Super キー (Windows キー / Cmd キー)。 */
    LeftSuper,
    /** 右 Super キー (Windows キー / Cmd キー)。 */
    RightSuper,

    /** 上矢印キー。 */
    Up,
    /** 下矢印キー。 */
    Down,
    /** 左矢印キー。 */
    Left,
    /** 右矢印キー。 */
    Right,

    /** スペースキー。 */
    Space,
    /** Enter キー。 */
    Enter,
    /** Tab キー。 */
    Tab,
    /** Backspace キー。 */
    Backspace,
    /** Escape キー。 */
    Escape,
    /** Insert キー。 */
    Insert,
    /** Delete キー。 */
    Delete,
    /** Home キー。 */
    Home,
    /** End キー。 */
    End,
    /** PageUp キー。 */
    PageUp,
    /** PageDown キー。 */
    PageDown,
    /** CapsLock キー。 */
    CapsLock,
    /** NumLock キー。 */
    NumLock,
    /** ScrollLock キー。 */
    ScrollLock,

    /** マイナス記号キー (-_)。 */
    Minus,
    /** イコール記号キー (=+)。 */
    Equal,
    /** 左角括弧キー ([{)。 */
    LeftBracket,
    /** 右角括弧キー (]})。 */
    RightBracket,
    /** バックスラッシュキー (\|)。 */
    Backslash,
    /** セミコロンキー (;:)。 */
    Semicolon,
    /** アポストロフィキー ('")。 */
    Apostrophe,
    /** カンマキー (,<)。 */
    Comma,
    /** ピリオドキー (.>)。 */
    Period,
    /** スラッシュキー (/?)。 */
    Slash,
    /** バッククォートキー (`~)。 */
    Grave,

    /** テンキー 0。 */
    KP0,
    /** テンキー 1。 */
    KP1,
    /** テンキー 2。 */
    KP2,
    /** テンキー 3。 */
    KP3,
    /** テンキー 4。 */
    KP4,
    /** テンキー 5。 */
    KP5,
    /** テンキー 6。 */
    KP6,
    /** テンキー 7。 */
    KP7,
    /** テンキー 8。 */
    KP8,
    /** テンキー 9。 */
    KP9,
    /** テンキーの加算キー (+)。 */
    KPAdd,
    /** テンキーの減算キー (-)。 */
    KPSubtract,
    /** テンキーの乗算キー (*)。 */
    KPMultiply,
    /** テンキーの除算キー (/)。 */
    KPDivide,
    /** テンキーの Enter キー。 */
    KPEnter,
    /** テンキーの小数点キー (.)。 */
    KPDecimal,

    /** キー総数 (配列サイズ用の番兵。実キーではない)。 */
    _Count
};

/** マウスのボタン識別子。 */
enum class EMouseButton : u8 {
    /** 左ボタン。 */
    Left   = 0,

    /** 右ボタン。 */
    Right  = 1,

    /** 中ボタン (ホイール押し込み)。 */
    Middle = 2,

    /** サイドボタン 1。 */
    X1     = 3,

    /** サイドボタン 2。 */
    X2     = 4,

    /** ボタン総数 (配列サイズ用の番兵。実ボタンではない)。 */
    _Count
};

/**
 * ゲームパッドのボタン識別子 (XInput 互換)。
 *
 * @details 値は kPadBits テーブル (Input.cpp) の添字に使うため連続。
 */
enum class EGamepadButton : u8 {
    /** A ボタン。 */
    A,
    /** B ボタン。 */
    B,
    /** X ボタン。 */
    X,
    /** Y ボタン。 */
    Y,

    /** 方向パッド上。 */
    Up,
    /** 方向パッド下。 */
    Down,
    /** 方向パッド左。 */
    Left,
    /** 方向パッド右。 */
    Right,

    /** 左バンパー (LB / L1)。 */
    LeftBumper,
    /** 右バンパー (RB / R1)。 */
    RightBumper,

    /** 左スティック押し込み。 */
    LeftStick,
    /** 右スティック押し込み。 */
    RightStick,

    /** Start ボタン。 */
    Start,
    /** Back ボタン。 */
    Back,
    /** Guide ボタン (XInput では未公開のため常に未検出)。 */
    Guide,

    /** ボタン総数 (配列サイズ用の番兵。実ボタンではない)。 */
    _Count
};

/** ゲームパッドのアナログ軸識別子 (スティックは -1.0〜+1.0、トリガーは 0.0〜1.0)。 */
enum class EGamepadAxis : u8 {
    /** 左スティックの X 軸。 */
    LeftX,
    /** 左スティックの Y 軸。 */
    LeftY,

    /** 右スティックの X 軸。 */
    RightX,
    /** 右スティックの Y 軸。 */
    RightY,

    /** 左トリガー (0.0〜1.0)。 */
    LeftTrigger,
    /** 右トリガー (0.0〜1.0)。 */
    RightTrigger,

    /** 軸総数 (配列サイズ用の番兵。実軸ではない)。 */
    _Count
};

} // namespace acs
