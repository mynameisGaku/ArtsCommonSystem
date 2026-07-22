// SPDX-License-Identifier: Apache-2.0
// ウィンドウ・入力イベント定義（FWindow がアプリにイベント通知するときに使う）
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"

namespace acs {

/** ウィンドウ・入力イベントの種別。 */
enum class EEventType : u8 {
    /** 無効値 (ゼロ初期化された FEvent の既定)。 */
    None = 0,

    /** ウィンドウの × ボタンや閉じる要求が来た。 */
    WindowClose,

    /** ウィンドウのクライアント領域サイズが変わった (resize フィールド有効)。 */
    WindowResize,

    /** ウィンドウがフォーカスを獲得した。 */
    WindowFocus,

    /** ウィンドウがフォーカスを喪失した。 */
    WindowLostFocus,

    /** キーが押された (押下開始の 1 回。key フィールド有効)。 */
    KeyPressed,

    /** キーが離された (key フィールド有効)。 */
    KeyReleased,

    /** キーが押され続けて OS のオートリピートが発火した (key フィールド有効)。 */
    KeyRepeat,

    /** マウスボタンが押された (mouse_button フィールド有効)。 */
    MouseButtonPressed,

    /** マウスボタンが離された (mouse_button フィールド有効)。 */
    MouseButtonReleased,

    /** マウスカーソルが移動した (mouse_move フィールド有効)。 */
    MouseMoved,

    /** マウスホイールが回転した (mouse_scroll フィールド有効)。 */
    MouseScrolled,

    /** 文字が入力された (IME 確定後の Unicode コードポイント。char_input フィールド有効)。 */
    CharInput,
};

/**
 * 1 件のウィンドウ・入力イベント。
 *
 * @details
 * type に応じて union 内の対応するメンバだけが有効になる。FWindow が WindowProc で
 * 構築し、SetEventCallback で登録されたコールバックへ渡す。
 */
struct FEvent {
    /** このイベントの種別 (どの union メンバが有効かを決める)。 */
    EEventType type = EEventType::None;

    union {
        /** WindowResize 用: 新しいクライアント領域サイズ (px)。 */
        struct { u32 width, height; }                    resize;

        /** KeyPressed / KeyReleased / KeyRepeat 用: 対象キーとリピートフラグ。 */
        struct { EKey key; bool repeat; }                 key;

        /** MouseButtonPressed / MouseButtonReleased 用: 対象ボタン。 */
        struct { EMouseButton button; }                   mouse_button;

        /** MouseMoved 用: カーソル位置 (x,y) と前回からの差分 (dx,dy)、クライアント座標。 */
        struct { f32 x, y; f32 dx, dy; }                 mouse_move;

        /** MouseScrolled 用: ホイール回転量 (x=水平, y=垂直)。 */
        struct { f32 x, y; }                             mouse_scroll;

        /** CharInput 用: 入力された Unicode コードポイント。 */
        struct { u32 codepoint; }                        char_input;
    };
};

} // namespace acs
