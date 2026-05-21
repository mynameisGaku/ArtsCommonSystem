// SPDX-License-Identifier: Apache-2.0
// ウィンドウ・入力イベント定義（Window がアプリにイベント通知するときに使う）
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"

namespace acs {

// イベント種別
enum class EventType : u8 {
    None = 0,
    WindowClose,         // ウィンドウの × ボタンが押された
    WindowResize,        // ウィンドウサイズが変わった
    WindowFocus,         // フォーカス獲得
    WindowLostFocus,     // フォーカス喪失
    KeyPressed,          // キーが押された (押下開始)
    KeyReleased,         // キーが離された
    KeyRepeat,           // キーが押され続けてリピート
    MouseButtonPressed,  // マウスボタン押下
    MouseButtonReleased, // マウスボタン解放
    MouseMoved,          // マウスカーソル移動
    MouseScrolled,       // ホイール回転
    CharInput,           // 文字入力 (テキスト入力用、IME 後)
};

// イベント本体（種別ごとに有効なフィールドが異なる union 風レイアウト）
struct Event {
    EventType type = EventType::None;
    union {
        struct { u32 width, height; }                    resize;        // WindowResize
        struct { Key key; bool repeat; }                 key;           // Key*
        struct { MouseButton button; }                   mouse_button;  // MouseButton*
        struct { f32 x, y; f32 dx, dy; }                 mouse_move;    // MouseMoved
        struct { f32 x, y; }                             mouse_scroll;  // MouseScrolled
        struct { u32 codepoint; }                        char_input;    // CharInput
    };
};

} // namespace acs
