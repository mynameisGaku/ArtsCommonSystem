// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/InputAxisOptions.h"
#include "gameframework/InputMap.h"

#include <limits>

using namespace acs;
using namespace acs::game;

ACS_TEST(InputAxisOptions, AppliesDeadZoneScaleAndInversion) {
    /** FInputAxisOptions の初期状態を境界条件として検証する値。 */
    const FInputAxisOptions options{0.2f, 2.0f, true};
    EXPECT_NEAR(options.Apply(0.1f), 0.0f, 1e-6f);
    EXPECT_NEAR(options.Apply(0.6f), -1.0f, 1e-6f);
    EXPECT_NEAR(options.Apply(-0.6f), 1.0f, 1e-6f);
}

ACS_TEST(InputAxisOptions, InvalidBindingLeavesMapUnchanged) {
    /** FInputMap の初期状態を境界条件として検証する値。 */
    FInputMap map;
    /** 処理前の状態を保持する比較基準。 */
    const u32 before = map.BindingCount();

    EXPECT_FALSE(map.TryBindGamepadAxis(FActionId{}, EGamepadAxis::LeftX, 0u, FInputAxisOptions{} ));
    EXPECT_FALSE(map.TryBindGamepadAxis(FActionId("Move"), static_cast<EGamepadAxis>(255u), 0u, FInputAxisOptions{} ));
    EXPECT_FALSE(map.TryBindGamepadAxis(FActionId("Move"), EGamepadAxis::LeftX, 4u, FInputAxisOptions{} ));
    EXPECT_FALSE(map.TryBindGamepadAxis(FActionId("Move"), EGamepadAxis::LeftX, 0u, FInputAxisOptions{1.0f, 1.0f, false} ));
    EXPECT_FALSE(map.TryBindGamepadAxis(FActionId("Move"), EGamepadAxis::LeftX, 0u, FInputAxisOptions{0.0f, std::numeric_limits<f32>::infinity(), false} ));

    EXPECT_EQ(map.BindingCount(), before);
}

ACS_TEST(InputAxisOptions, ValidBindingCommitsOnce) {
    /** FInputMap の初期状態を境界条件として検証する値。 */
    FInputMap map;
    EXPECT_TRUE(map.TryBindGamepadAxis(FActionId("Move"), EGamepadAxis::RightX, 3u, FInputAxisOptions{0.15f, 1.5f, false} ));
    EXPECT_EQ(map.BindingCount(), 1u);
}
