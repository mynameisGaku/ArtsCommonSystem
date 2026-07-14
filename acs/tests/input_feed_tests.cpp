// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// 入力フィード経路の検証 (GPU 非依存): acs::Input::OnEvent でキーイベントを流すと、
//   poll ベースの FInputMap が IsHeld/Axis に正しく反映することを確認する。
//   = editor インプロセス Play で «WPF→editor_abi→reflect DLL の Input::OnEvent» と
//     フィードした入力が、ユーザーコンポーネントの Services().Input() に届く土台。
//   注意: acs::Input はプロセスグローバルなので、各テストは触ったキーを最後に解放する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "platform/Input.h"
#include "platform/Event.h"
#include "platform/InputCodes.h"
#include "gameframework/InputMap.h"

using namespace acs;
using namespace acs::game;

namespace {
Event KeyEvt(EKey k, bool down) noexcept {
    Event e;
    e.type = down ? EventType::KeyPressed : EventType::KeyReleased;
    e.key.key = k;
    e.key.repeat = false;
    return e;
}
} // namespace

// OnEvent で押下/解放を流すと FInputMap.IsHeld が追従する。
ACS_TEST(InputFeed, OnEventDrivesInputMapHeld) {
    Input::Update();   // クリーンなフレーム境界
    FInputMap im;
    im.BindKey(ActionId("Jump"), EKey::Space);

    EXPECT_FALSE(im.IsHeld(ActionId("Jump")));
    Input::OnEvent(KeyEvt(EKey::Space, true));
    EXPECT_TRUE(im.IsHeld(ActionId("Jump")));     // フィードした入力が poll に届く
    Input::OnEvent(KeyEvt(EKey::Space, false));
    EXPECT_FALSE(im.IsHeld(ActionId("Jump")));    // 解放で戻る
}

// 1D axis: neg/pos キーをフィードすると -1 / +1 / 0 を返す。
ACS_TEST(InputFeed, AxisFromFedKeys) {
    Input::Update();
    FInputMap im;
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);

    EXPECT_NEAR(im.Axis(ActionId("MoveX")), 0.0f, 1e-4f);
    Input::OnEvent(KeyEvt(EKey::D, true));
    EXPECT_NEAR(im.Axis(ActionId("MoveX")), 1.0f, 1e-4f);     // +X
    Input::OnEvent(KeyEvt(EKey::A, true));
    EXPECT_NEAR(im.Axis(ActionId("MoveX")), 0.0f, 1e-4f);     // 両押しは相殺
    Input::OnEvent(KeyEvt(EKey::D, false));
    EXPECT_NEAR(im.Axis(ActionId("MoveX")), -1.0f, 1e-4f);    // -X
    Input::OnEvent(KeyEvt(EKey::A, false));                    // 後始末
    EXPECT_NEAR(im.Axis(ActionId("MoveX")), 0.0f, 1e-4f);
}

// マウスボタンを OnEvent で流すと FInputMap (BindMouseButton) が追従する。
ACS_TEST(InputFeed, MouseButtonFeedsInputMap) {
    Input::Update();
    FInputMap im;
    im.BindMouseButton(ActionId("Shoot"), EMouseButton::Left);

    EXPECT_FALSE(im.IsHeld(ActionId("Shoot")));
    Event d; d.type = EventType::MouseButtonPressed;  d.mouse_button.button = EMouseButton::Left;
    Input::OnEvent(d);
    EXPECT_TRUE(im.IsHeld(ActionId("Shoot")));     // フィードした左クリックが poll に届く
    Event u; u.type = EventType::MouseButtonReleased; u.mouse_button.button = EMouseButton::Left;
    Input::OnEvent(u);
    EXPECT_FALSE(im.IsHeld(ActionId("Shoot")));
}

// IsPressed の立ち上がりエッジは Update() (now→prev ロール) を挟むと検出される。
ACS_TEST(InputFeed, PressedEdgeAfterUpdate) {
    Input::Update();
    FInputMap im;
    im.BindKey(ActionId("Fire"), EKey::W);

    Input::OnEvent(KeyEvt(EKey::W, true));         // 押下 (now=true, prev は前フレーム)
    EXPECT_TRUE(im.IsHeld(ActionId("Fire")));
    Input::Update();                                // now→prev (prev=true)
    EXPECT_FALSE(im.IsPressed(ActionId("Fire")));   // もう「このフレーム押下開始」ではない
    EXPECT_TRUE(im.IsHeld(ActionId("Fire")));       // 押しっぱなしは Held のまま
    Input::OnEvent(KeyEvt(EKey::W, false));         // 後始末
    Input::Update();
}

// 未接続扱いのplayerと不正なbutton値は、安全な既定値を返す。
ACS_TEST(InputFeed, InvalidGamepadInputIsRejected)
{
    constexpr u32 invalid_player = 99;
    const auto invalid_button = static_cast<EGamepadButton>(255);
    EXPECT_FALSE(Input::IsGamepadConnected(invalid_player));
    EXPECT_FALSE(Input::IsGamepadButtonDown(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(Input::IsGamepadButtonPressed(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(Input::IsGamepadButtonReleased(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(Input::IsGamepadButtonDown(0, invalid_button));
    EXPECT_FALSE(Input::IsGamepadButtonPressed(0, invalid_button));
    EXPECT_FALSE(Input::IsGamepadButtonReleased(0, invalid_button));
}

// analog bindingが存在しても、他のaxis bindingとの合成規則を維持する。
ACS_TEST(InputFeed, GamepadAxisBindingComposesWithKeys)
{
    Input::Update();
    FInputMap input_map;
    const ActionId move("MoveX");
    input_map.BindAxisKeys(move, EKey::A, EKey::D);
    input_map.BindGamepadAxis(move, EGamepadAxis::LeftX, 99, 0.5f);

    EXPECT_NEAR(input_map.Axis(move), 0.0f, 1e-4f);
    EXPECT_FALSE(input_map.IsHeld(move));
    Input::OnEvent(KeyEvt(EKey::D, true));
    EXPECT_NEAR(input_map.Axis(move), 1.0f, 1e-4f);
    EXPECT_TRUE(input_map.IsHeld(move));
    Input::OnEvent(KeyEvt(EKey::D, false));
}
