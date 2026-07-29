// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// 入力フィード経路の検証 (GPU 非依存): acs::FInput::OnEvent でキーイベントを流すと、
//   poll ベースの FInputMap が IsHeld/Axis に正しく反映することを確認する。
//   = editor インプロセス Play で «WPF→editor_abi→reflect DLL の FInput::OnEvent» と
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
FEvent KeyEvt(EKey k, bool down) noexcept {
    FEvent e;
    e.type = down ? EEventType::KeyPressed : EEventType::KeyReleased;
    e.key.key = k;
    e.key.repeat = false;
    return e;
}
} // namespace

// OnEvent で押下/解放を流すと FInputMap.IsHeld が追従する。
ACS_TEST(InputFeed, OnEventDrivesInputMapHeld) {
    FInput::Update();   // クリーンなフレーム境界
    FInputMap im;
    im.BindKey(FActionId("Jump"), EKey::Space);

    EXPECT_FALSE(im.IsHeld(FActionId("Jump")));
    FInput::OnEvent(KeyEvt(EKey::Space, true));
    EXPECT_TRUE(im.IsHeld(FActionId("Jump")));     // フィードした入力が poll に届く
    FInput::OnEvent(KeyEvt(EKey::Space, false));
    EXPECT_FALSE(im.IsHeld(FActionId("Jump")));    // 解放で戻る
}

// 1D axis: neg/pos キーをフィードすると -1 / +1 / 0 を返す。
ACS_TEST(InputFeed, AxisFromFedKeys) {
    FInput::Update();
    FInputMap im;
    im.BindAxisKeys(FActionId("MoveX"), EKey::A, EKey::D);

    EXPECT_NEAR(im.Axis(FActionId("MoveX")), 0.0f, 1e-4f);
    FInput::OnEvent(KeyEvt(EKey::D, true));
    EXPECT_NEAR(im.Axis(FActionId("MoveX")), 1.0f, 1e-4f);     // +X
    FInput::OnEvent(KeyEvt(EKey::A, true));
    EXPECT_NEAR(im.Axis(FActionId("MoveX")), 0.0f, 1e-4f);     // 両押しは相殺
    FInput::OnEvent(KeyEvt(EKey::D, false));
    EXPECT_NEAR(im.Axis(FActionId("MoveX")), -1.0f, 1e-4f);    // -X
    FInput::OnEvent(KeyEvt(EKey::A, false));                    // 後始末
    EXPECT_NEAR(im.Axis(FActionId("MoveX")), 0.0f, 1e-4f);
}

// マウスボタンを OnEvent で流すと FInputMap (BindMouseButton) が追従する。
ACS_TEST(InputFeed, MouseButtonFeedsInputMap) {
    FInput::Update();
    FInputMap im;
    im.BindMouseButton(FActionId("Shoot"), EMouseButton::Left);

    EXPECT_FALSE(im.IsHeld(FActionId("Shoot")));
    FEvent d; d.type = EEventType::MouseButtonPressed;  d.mouse_button.button = EMouseButton::Left;
    FInput::OnEvent(d);
    EXPECT_TRUE(im.IsHeld(FActionId("Shoot")));     // フィードした左クリックが poll に届く
    FEvent u; u.type = EEventType::MouseButtonReleased; u.mouse_button.button = EMouseButton::Left;
    FInput::OnEvent(u);
    EXPECT_FALSE(im.IsHeld(FActionId("Shoot")));
}

// IsPressed の立ち上がりエッジは Update() (now→prev ロール) を挟むと検出される。
ACS_TEST(InputFeed, PressedEdgeAfterUpdate) {
    FInput::Update();
    FInputMap im;
    im.BindKey(FActionId("Fire"), EKey::W);

    FInput::OnEvent(KeyEvt(EKey::W, true));         // 押下 (now=true, prev は前フレーム)
    EXPECT_TRUE(im.IsHeld(FActionId("Fire")));
    FInput::Update();                                // now→prev (prev=true)
    EXPECT_FALSE(im.IsPressed(FActionId("Fire")));   // もう「このフレーム押下開始」ではない
    EXPECT_TRUE(im.IsHeld(FActionId("Fire")));       // 押しっぱなしは Held のまま
    FInput::OnEvent(KeyEvt(EKey::W, false));         // 後始末
    FInput::Update();
}

// 未接続扱いのplayerと不正なbutton値は、安全な既定値を返す。
ACS_TEST(InputFeed, InvalidGamepadInputIsRejected)
{
    constexpr u32 invalid_player = 99;
    const auto invalid_button = static_cast<EGamepadButton>(255);
    EXPECT_FALSE(FInput::IsGamepadConnected(invalid_player));
    EXPECT_FALSE(FInput::IsGamepadButtonDown(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(FInput::IsGamepadButtonPressed(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(FInput::IsGamepadButtonReleased(invalid_player, EGamepadButton::A));
    EXPECT_FALSE(FInput::IsGamepadButtonDown(0, invalid_button));
    EXPECT_FALSE(FInput::IsGamepadButtonPressed(0, invalid_button));
    EXPECT_FALSE(FInput::IsGamepadButtonReleased(0, invalid_button));
}

// analog bindingが存在しても、他のaxis bindingとの合成規則を維持する。
ACS_TEST(InputFeed, GamepadAxisBindingComposesWithKeys)
{
    FInput::Update();
    FInputMap input_map;
    const FActionId move("MoveX");
    input_map.BindAxisKeys(move, EKey::A, EKey::D);
    input_map.BindGamepadAxis(move, EGamepadAxis::LeftX, 99, 0.5f);

    EXPECT_NEAR(input_map.Axis(move), 0.0f, 1e-4f);
    EXPECT_FALSE(input_map.IsHeld(move));
    FInput::OnEvent(KeyEvt(EKey::D, true));
    EXPECT_NEAR(input_map.Axis(move), 1.0f, 1e-4f);
    EXPECT_TRUE(input_map.IsHeld(move));
    FInput::OnEvent(KeyEvt(EKey::D, false));
}

// 全ポート未接続時は1フレーム1回だけ確認し、4フレーム以内に全ポートを巡回する。
ACS_TEST(InputFeed, DisconnectedGamepadPollingIsStaggered)
{
    bool connected[4]{};
    detail::TGamepadPollScheduler<4> scheduler;
    EXPECT_EQ(scheduler.BuildPollMask(connected), 0x1u);
    EXPECT_EQ(scheduler.BuildPollMask(connected), 0x2u);
    EXPECT_EQ(scheduler.BuildPollMask(connected), 0x4u);
    EXPECT_EQ(scheduler.BuildPollMask(connected), 0x8u);
    EXPECT_EQ(scheduler.BuildPollMask(connected), 0x1u);
}

// 接続中ポートは毎フレーム含め、未接続確認だけを追加で1ポートに制限する。
ACS_TEST(InputFeed, ConnectedGamepadPollingRemainsFullRate)
{
    bool connected[4]{true, false, true, false};
    detail::TGamepadPollScheduler<4> scheduler;
    const u32 first = scheduler.BuildPollMask(connected);
    const u32 second = scheduler.BuildPollMask(connected);
    EXPECT_EQ(first & 0x5u, 0x5u);
    EXPECT_EQ(second & 0x5u, 0x5u);
    EXPECT_EQ(first | second, 0xFu);
}
