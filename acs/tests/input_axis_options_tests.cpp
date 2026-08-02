// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "gameframework/InputAxisOptions.h"
#include "gameframework/InputMap.h"
#include "platform/Event.h"
#include "platform/Input.h"

#include <cstddef>
#include <cmath>
#include <limits>
#include <type_traits>

using namespace acs;
using namespace acs::game;

namespace {

/** キー入力の押下または解放eventを作る。 */
FEvent MakeKeyEvent(EKey key, bool down) noexcept
{
    /** 入力mapへ渡すキーevent。 */
    FEvent event{};
    event.type = down ? EEventType::KeyPressed : EEventType::KeyReleased;
    event.key.key = key;
    event.key.repeat = false;
    return event;
}

static_assert(sizeof(FInputAxisOptions) == 12u);
static_assert(alignof(FInputAxisOptions) == 4u);
static_assert(offsetof(FInputAxisOptions, dead_zone) == 0u);
static_assert(offsetof(FInputAxisOptions, scale) == 4u);
static_assert(offsetof(FInputAxisOptions, inverted) == 8u);
static_assert(std::is_aggregate_v<FInputAxisOptions>);
static_assert(std::is_standard_layout_v<FInputAxisOptions>);
static_assert(std::is_trivially_copyable_v<FInputAxisOptions>);
static_assert(noexcept(FInputAxisOptions{}.Apply(0.0f)));
static_assert(std::is_same_v<decltype(&FInputMap::Axis), f32 (FInputMap::*)(FActionId) const noexcept>);
static_assert(std::is_same_v<decltype(&FInputMap::AxisValue), f32 (FInputMap::*)(FActionId, FInputAxisOptions) const noexcept>);
static_assert(std::is_same_v<decltype(&FInputMap::BindGamepadAxis), void (FInputMap::*)(FActionId, EGamepadAxis, u32, f32) noexcept>);
static_assert(std::is_same_v<decltype(&FInputMap::IsHeld), bool (FInputMap::*)(FActionId) const noexcept>);

} // namespace

ACS_TEST(InputAxisOptions, DefaultsPreserveFiniteAxisAndClampRange)
{
    /** 既定設定の入力補正。 */
    const FInputAxisOptions options{};
    EXPECT_NEAR(options.Apply(-2.0f), -1.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(-0.25f), -0.25f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(0.0f), 0.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(0.5f), 0.5f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(2.0f), 1.0f, 1.0e-6f);
}

ACS_TEST(InputAxisOptions, RejectsInvalidOptionsAndNonFiniteInput)
{
    /** 不正値を順番に設定する入力補正。 */
    FInputAxisOptions options{};
    options.dead_zone = -0.001f;
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.dead_zone = 1.0f;
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.dead_zone = 1.1f;
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.dead_zone = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.dead_zone = std::numeric_limits<f32>::infinity();
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);

    options = {};
    options.scale = -0.001f;
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.scale = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);
    options.scale = std::numeric_limits<f32>::infinity();
    EXPECT_NEAR(options.Apply(0.5f), 0.0f, 1.0e-6f);

    options = {};
    EXPECT_NEAR(options.Apply(std::numeric_limits<f32>::quiet_NaN()), 0.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(std::numeric_limits<f32>::infinity()), 0.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(-std::numeric_limits<f32>::infinity()), 0.0f, 1.0e-6f);
}

ACS_TEST(InputAxisOptions, RenormalizesDeadZoneInclusively)
{
    /** 四分の一を無入力範囲にする入力補正。 */
    const FInputAxisOptions options{0.25f, 1.0f, false};
    EXPECT_NEAR(options.Apply(0.25f), 0.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(-0.25f), 0.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(0.625f), 0.5f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(-0.625f), -0.5f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(1.0f), 1.0f, 1.0e-6f);
    EXPECT_NEAR(options.Apply(-1.0f), -1.0f, 1.0e-6f);
}

ACS_TEST(InputAxisOptions, AppliesScaleClampAndDirectionXor)
{
    /** 倍率と反転を同時に確認する入力補正。 */
    const FInputAxisOptions inverted{0.2f, 0.5f, true};
    EXPECT_NEAR(inverted.Apply(0.6f), -0.25f, 1.0e-6f);
    EXPECT_NEAR(inverted.Apply(-0.6f), 0.25f, 1.0e-6f);

    /** 出力を上限へ制限する入力補正。 */
    const FInputAxisOptions amplified{0.25f, 4.0f, false};
    EXPECT_NEAR(amplified.Apply(0.625f), 1.0f, 1.0e-6f);
    EXPECT_NEAR(amplified.Apply(-0.625f), -1.0f, 1.0e-6f);

    /** 最大有限倍率を二倍精度で計算する入力補正。 */
    const FInputAxisOptions maximum_scale{0.25f, std::numeric_limits<f32>::max(), false};
    EXPECT_NEAR(maximum_scale.Apply(std::nextafter(0.25f, 1.0f)), 1.0f, 1.0e-6f);

    /** 有効だが常に0を返す倍率設定。 */
    const FInputAxisOptions zero_scale{0.0f, 0.0f, true};
    EXPECT_NEAR(zero_scale.Apply(-1.0f), 0.0f, 1.0e-6f);
}

ACS_TEST(InputAxisOptions, InputMapAxisValueOnlyPostProcessesExistingAxis)
{
    CInput::OnEvent(MakeKeyEvent(EKey::D, false));
    CInput::Update();

    /** 既存axis経路と新しい補正経路を比較する入力map。 */
    FInputMap input_map;
    /** 試験用の移動action。 */
    const FActionId move("MoveX");
    input_map.BindAxisKeys(move, EKey::A, EKey::D);
    CInput::OnEvent(MakeKeyEvent(EKey::D, true));

    /** 既存値へ四分の一倍率と反転を適用する設定。 */
    const FInputAxisOptions options{0.0f, 0.25f, true};
    EXPECT_NEAR(input_map.Axis(move), 1.0f, 1.0e-6f);
    EXPECT_TRUE(input_map.IsHeld(move));
    EXPECT_NEAR(input_map.AxisValue(move, options), options.Apply(input_map.Axis(move)), 1.0e-6f);
    EXPECT_NEAR(input_map.AxisValue(move, options), -0.25f, 1.0e-6f);

    CInput::OnEvent(MakeKeyEvent(EKey::D, false));
    EXPECT_NEAR(input_map.Axis(move), 0.0f, 1.0e-6f);
    EXPECT_FALSE(input_map.IsHeld(move));
}
