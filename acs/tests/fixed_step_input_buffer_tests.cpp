// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepInputBuffer.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** 非有限なゲームパッド軸を返し、入力全体の拒否を再現する参照。 */
class FNonFiniteAxisInput final : public IInputStateView {
public:
    /** キー保持はない。 */
    bool IsKeyDown(EKey) const noexcept override
    {
        return false;
    }
    /** キー押下はない。 */
    bool IsKeyPressed(EKey) const noexcept override
    {
        return false;
    }
    /** キー解放はない。 */
    bool IsKeyReleased(EKey) const noexcept override
    {
        return false;
    }
    /** マウス保持はない。 */
    bool IsMouseButtonDown(EMouseButton) const noexcept override
    {
        return false;
    }
    /** マウス押下はない。 */
    bool IsMouseButtonPressed(EMouseButton) const noexcept override
    {
        return false;
    }
    /** マウス解放はない。 */
    bool IsMouseButtonReleased(EMouseButton) const noexcept override
    {
        return false;
    }
    /** ゲームパッド保持はない。 */
    bool IsGamepadButtonDown(u32, EGamepadButton) const noexcept override
    {
        return false;
    }
    /** ゲームパッド押下はない。 */
    bool IsGamepadButtonPressed(u32, EGamepadButton) const noexcept override
    {
        return false;
    }
    /** ゲームパッド解放はない。 */
    bool IsGamepadButtonReleased(u32, EGamepadButton) const noexcept override
    {
        return false;
    }

    /** 先頭プレイヤーの左X軸だけ非有限値を返す。 */
    f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept override
    {
        return player_index == 0u && axis == EGamepadAxis::LeftX ? std::numeric_limits<f32>::quiet_NaN() : 0.0f;
    }
};

} // namespace

ACS_TEST(FixedStepInputBuffer, FirstFixedStepConsumesEdgesOnlyOnce)
{
    /** 可変フレーム入力を固定更新へ渡すbuffer。 */
    FFixedStepInputBuffer buffer;
    /** 押下を含む一フレーム分の入力。 */
    FInputStateSnapshot frame;
    EXPECT_TRUE(frame.TrySetKeyState(EKey::Space, true, true, false));
    EXPECT_TRUE(buffer.TryPushFrame(frame));
    EXPECT_TRUE(buffer.HasInputState());

    /** 最初の固定更新へ渡す入力。 */
    FInputStateSnapshot first_tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(first_tick));
    EXPECT_TRUE(first_tick.IsKeyDown(EKey::Space));
    EXPECT_TRUE(first_tick.IsKeyPressed(EKey::Space));
    EXPECT_FALSE(first_tick.IsKeyReleased(EKey::Space));

    /** 同じ描画フレーム内の二回目の固定更新へ渡す入力。 */
    FInputStateSnapshot second_tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(second_tick));
    EXPECT_TRUE(second_tick.IsKeyDown(EKey::Space));
    EXPECT_FALSE(second_tick.IsKeyPressed(EKey::Space));
    EXPECT_FALSE(second_tick.IsKeyReleased(EKey::Space));
}

ACS_TEST(FixedStepInputBuffer, ShortTapSurvivesFramesWithoutFixedStep)
{
    /** 固定更新がまだ走らない間も入力を保持するbuffer。 */
    FFixedStepInputBuffer buffer;
    /** 押下フレーム。 */
    FInputStateSnapshot pressed_frame;
    EXPECT_TRUE(pressed_frame.TrySetKeyState(EKey::Enter, true, true, false));
    EXPECT_TRUE(buffer.TryPushFrame(pressed_frame));

    /** 固定更新より前に同じキーを離したフレーム。 */
    FInputStateSnapshot released_frame;
    EXPECT_TRUE(released_frame.TrySetKeyState(EKey::Enter, false, false, true));
    EXPECT_TRUE(buffer.TryPushFrame(released_frame));

    /** 次の固定更新で観測する短いタップ。 */
    FInputStateSnapshot tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(tick));
    EXPECT_FALSE(tick.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(tick.IsKeyPressed(EKey::Enter));
    EXPECT_TRUE(tick.IsKeyReleased(EKey::Enter));
}

ACS_TEST(FixedStepInputBuffer, LatestLevelsAndAxesWinWhileEdgesAccumulate)
{
    /** 複数種類の入力を合成するbuffer。 */
    FFixedStepInputBuffer buffer;
    /** 最初の入力状態。 */
    FInputStateSnapshot first_frame;
    EXPECT_TRUE(first_frame.TrySetMouseButtonState(EMouseButton::Left, true, true, false));
    EXPECT_TRUE(first_frame.TrySetGamepadButtonState(0u, EGamepadButton::South, true, true, false));
    EXPECT_TRUE(first_frame.TrySetGamepadAxis(0u, EGamepadAxis::LeftX, 0.25f));
    EXPECT_TRUE(buffer.TryPushFrame(first_frame));

    /** 最新の保持状態と軸を更新する入力状態。 */
    FInputStateSnapshot latest_frame;
    EXPECT_TRUE(latest_frame.TrySetMouseButtonState(EMouseButton::Left, true, false, false));
    EXPECT_TRUE(latest_frame.TrySetGamepadButtonState(0u, EGamepadButton::South, false, false, true));
    EXPECT_TRUE(latest_frame.TrySetGamepadAxis(0u, EGamepadAxis::LeftX, -0.5f));
    EXPECT_TRUE(buffer.TryPushFrame(latest_frame));

    /** 蓄積したエッジと最新値を含む固定更新入力。 */
    FInputStateSnapshot tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(tick));
    EXPECT_TRUE(tick.IsMouseButtonDown(EMouseButton::Left));
    EXPECT_TRUE(tick.IsMouseButtonPressed(EMouseButton::Left));
    EXPECT_FALSE(tick.IsMouseButtonReleased(EMouseButton::Left));
    EXPECT_FALSE(tick.IsGamepadButtonDown(0u, EGamepadButton::South));
    EXPECT_TRUE(tick.IsGamepadButtonPressed(0u, EGamepadButton::South));
    EXPECT_TRUE(tick.IsGamepadButtonReleased(0u, EGamepadButton::South));
    EXPECT_NEAR(tick.GamepadAxisValue(0u, EGamepadAxis::LeftX), -0.5f, 1.0e-6f);
}

ACS_TEST(FixedStepInputBuffer, InvalidFramePreservesPendingState)
{
    /** 不正入力の前後で状態を比較するbuffer。 */
    FFixedStepInputBuffer buffer;
    /** 失敗後も残す有効な入力。 */
    FInputStateSnapshot valid_frame;
    EXPECT_TRUE(valid_frame.TrySetKeyState(EKey::Space, true, true, false));
    EXPECT_TRUE(valid_frame.TrySetGamepadAxis(0u, EGamepadAxis::LeftX, 0.4f));
    EXPECT_TRUE(buffer.TryPushFrame(valid_frame));

    /** 非有限な軸を返す不正入力。 */
    const FNonFiniteAxisInput invalid_frame;
    EXPECT_FALSE(buffer.TryPushFrame(invalid_frame));

    /** 失敗前の状態を取得する固定更新入力。 */
    FInputStateSnapshot tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(tick));
    EXPECT_TRUE(tick.IsKeyDown(EKey::Space));
    EXPECT_TRUE(tick.IsKeyPressed(EKey::Space));
    EXPECT_NEAR(tick.GamepadAxisValue(0u, EGamepadAxis::LeftX), 0.4f, 1.0e-6f);
}

ACS_TEST(FixedStepInputBuffer, ResetAndUninitializedConsumePreserveOutput)
{
    /** 未初期化とReset後の挙動を確認するbuffer。 */
    FFixedStepInputBuffer buffer;
    /** 失敗時に変更されないことを確認する出力。 */
    FInputStateSnapshot output;
    EXPECT_TRUE(output.TrySetKeyState(EKey::Enter, true, true, false));

    EXPECT_FALSE(buffer.TryConsumeFixedStep(output));
    EXPECT_TRUE(output.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(output.IsKeyPressed(EKey::Enter));

    /** 初期化に使う空入力。 */
    const FInputStateSnapshot empty_frame;
    EXPECT_TRUE(buffer.TryPushFrame(empty_frame));
    buffer.Reset();
    EXPECT_FALSE(buffer.HasInputState());
    EXPECT_FALSE(buffer.TryConsumeFixedStep(output));
    EXPECT_TRUE(output.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(output.IsKeyPressed(EKey::Enter));
}
