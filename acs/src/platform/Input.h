// SPDX-License-Identifier: Apache-2.0
// 入力ポーリング API（キーボード / マウス / ゲームパッド）
//
// 使い方:
//   while (!w.ShouldClose()) {
//       Input::Update();           // フレーム先頭で呼ぶ
//       w.PollEvents();
//
//       if (Input::IsKeyDown(Key::Space)) Jump();
//       if (Input::IsMouseButtonPressed(MouseButton::Left)) Shoot();
//       Vec2 m = Input::MousePos();
//   }
//
// ・「Down」 = 現在押されている
// ・「Pressed」 = このフレームで押された (前フレームは Down ではなかった)
// ・「Released」 = このフレームで離された
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "platform/InputCodes.h"
#include "platform/Event.h"

namespace acs {

class Input {
public:
    // フレーム先頭で 1 回呼ぶ（前フレーム状態を更新する）
    static void Update() noexcept;

    // Window から受け取ったイベントを Input に流し込む
    // Window::SetEventCallback で OnEvent 関数をブリッジする
    static void OnEvent(const Event& e) noexcept;

    // キーボード
    static bool IsKeyDown(Key k) noexcept;       // 押されているか
    static bool IsKeyPressed(Key k) noexcept;     // このフレームで押されたか
    static bool IsKeyReleased(Key k) noexcept;    // このフレームで離されたか

    // マウス
    static bool IsMouseButtonDown(MouseButton b) noexcept;
    static bool IsMouseButtonPressed(MouseButton b) noexcept;
    static bool IsMouseButtonReleased(MouseButton b) noexcept;
    static Vec2 MousePos() noexcept;             // 現在の位置（クライアント座標）
    static Vec2 MouseDelta() noexcept;            // 前フレームからの移動量
    static f32  MouseWheel() noexcept;            // このフレームのホイール回転（正:奥, 負:手前）

    // テキスト入力（このフレームに入力された文字列、UTF-8、IME 確定後。無ければ ""）
    static const char* TextInput() noexcept;

    // ゲームパッド (XInput、player_index = 0..3)
    static bool IsGamepadConnected(u32 player_index) noexcept;
    static bool IsGamepadButtonDown(u32 player_index, GamepadButton b) noexcept;
    static bool IsGamepadButtonPressed(u32 player_index, GamepadButton b) noexcept;
    static f32  GamepadAxisValue(u32 player_index, GamepadAxis axis) noexcept;
};

} // namespace acs
