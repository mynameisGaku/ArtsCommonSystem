// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Game.h"
#include "gameframework/Scene.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

namespace {

/** FGame から受け取った固定更新と可変更新を記録する検証用シーン。 */
class AGameFixedStepProbeScene final : public FScene {
public:
    /** 固定更新の回数と刻みを記録する。 */
    void OnFixedUpdate(f32 fixed_delta_seconds) noexcept override
    {
        ++m_FixedUpdateCount;
        m_LastFixedDeltaSeconds = fixed_delta_seconds;
    }

    /** 可変更新の回数と経過秒を記録する。 */
    void OnUpdate(f32 delta_seconds) noexcept override
    {
        ++m_UpdateCount;
        m_LastUpdateDeltaSeconds = delta_seconds;
    }

    /** 記録した固定更新回数を返す。 */
    u32 FixedUpdateCount() const noexcept
    {
        return m_FixedUpdateCount;
    }

    /** 記録した可変更新回数を返す。 */
    u32 UpdateCount() const noexcept
    {
        return m_UpdateCount;
    }

    /** 最後に受け取った固定刻みを返す。 */
    f32 LastFixedDeltaSeconds() const noexcept
    {
        return m_LastFixedDeltaSeconds;
    }

    /** 最後に受け取った可変経過秒を返す。 */
    f32 LastUpdateDeltaSeconds() const noexcept
    {
        return m_LastUpdateDeltaSeconds;
    }

private:
    /** 固定更新が呼ばれた回数。 */
    u32 m_FixedUpdateCount = 0u;

    /** 可変更新が呼ばれた回数。 */
    u32 m_UpdateCount = 0u;

    /** 最後に受け取った固定刻み。 */
    f32 m_LastFixedDeltaSeconds = 0.0f;

    /** 最後に受け取った可変経過秒。 */
    f32 m_LastUpdateDeltaSeconds = 0.0f;
};

/** GPU を起動せず FGame のフレーム駆動を検証するアダプター。 */
class AGameFixedStepProbe final : public FGame {
public:
    /** 初期シーンを取り付ける。 */
    void StartForTest() noexcept
    {
        OnStart();
    }

    /** 指定した経過秒で 1 フレーム進める。 */
    void UpdateForTest(f32 delta_seconds) noexcept
    {
        OnUpdate(delta_seconds);
    }

    /** シーンとサブシステムを終了する。 */
    void ShutdownForTest() noexcept
    {
        OnShutdown();
    }

    /** 取り付けた検証用シーンを返す。 */
    AGameFixedStepProbeScene* SceneForTest() const noexcept
    {
        return m_Scene;
    }

protected:
    /** 固定更新を記録する初期シーンを生成する。 */
    TUniquePtr<FScene> InitialScene() noexcept override
    {
        TUniquePtr<AGameFixedStepProbeScene> scene = MakeUnique<AGameFixedStepProbeScene>();
        m_Scene = scene.Get();
        return TUniquePtr<FScene>(Move(scene));
    }

private:
    /** FSceneManager が所有する検証用シーンへの非所有参照。 */
    AGameFixedStepProbeScene* m_Scene = nullptr;
};

/** 2 つの固定更新 snapshot が同じ状態かを返す。 */
bool SameFixedStepSnapshot(const FFixedStepClockSnapshot& first, const FFixedStepClockSnapshot& second) noexcept
{
    return first.options.step_seconds == second.options.step_seconds &&
           first.options.maximum_steps_per_advance == second.options.maximum_steps_per_advance &&
           first.options.maximum_accumulated_seconds == second.options.maximum_accumulated_seconds &&
           first.accumulated_seconds == second.accumulated_seconds &&
           first.total_dropped_seconds == second.total_dropped_seconds &&
           first.total_step_count == second.total_step_count;
}

} // namespace

ACS_TEST(GameFixedStepIntegration, DrivesBoundedSceneUpdatesAndCapturesState)
{
    AGameFixedStepProbe game;
    FFixedStepOptions options{};
    options.step_seconds = 0.1;
    options.maximum_steps_per_advance = 2u;
    options.maximum_accumulated_seconds = 0.3;
    EXPECT_TRUE(game.TrySetFixedTimestep(options));

    game.StartForTest();
    AGameFixedStepProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    game.UpdateForTest(0.25f);
    EXPECT_EQ(scene->FixedUpdateCount(), 2u);
    EXPECT_EQ(scene->UpdateCount(), 1u);
    EXPECT_NEAR(scene->LastFixedDeltaSeconds(), 0.1f, 1.0e-6f);
    EXPECT_NEAR(scene->LastUpdateDeltaSeconds(), 0.25f, 1.0e-6f);

    FFixedStepClockSnapshot first_snapshot{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(first_snapshot));
    EXPECT_EQ(first_snapshot.total_step_count, 2u);
    EXPECT_NEAR(first_snapshot.accumulated_seconds, 0.05, 1.0e-6);
    EXPECT_NEAR(game.FixedStepInterpolationAlpha(), 0.5, 1.0e-6);

    game.UpdateForTest(0.5f);
    EXPECT_EQ(scene->FixedUpdateCount(), 4u);
    FFixedStepClockSnapshot clamped_snapshot{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(clamped_snapshot));
    EXPECT_EQ(clamped_snapshot.total_step_count, 4u);
    EXPECT_TRUE(clamped_snapshot.total_dropped_seconds > 0.34);

    EXPECT_TRUE(game.TryRestoreFixedStepSnapshot(first_snapshot));
    FFixedStepClockSnapshot restored_snapshot{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(restored_snapshot));
    EXPECT_TRUE(SameFixedStepSnapshot(first_snapshot, restored_snapshot));
    game.ShutdownForTest();
}

ACS_TEST(GameFixedStepIntegration, PreservesRestoredClockAcrossStart)
{
    FFixedStepClock source_clock;
    FFixedStepOptions options{};
    options.step_seconds = 0.1;
    options.maximum_steps_per_advance = 2u;
    options.maximum_accumulated_seconds = 0.3;
    EXPECT_TRUE(source_clock.Configure(options));
    const FFixedStepAdvanceResult advance = source_clock.Advance(0.15);
    EXPECT_TRUE(advance.accepted);
    EXPECT_EQ(advance.step_count, 1u);

    FFixedStepClockSnapshot restored_before_start{};
    EXPECT_TRUE(source_clock.TryCaptureSnapshot(restored_before_start));

    AGameFixedStepProbe game;
    EXPECT_TRUE(game.TryRestoreFixedStepSnapshot(restored_before_start));
    game.StartForTest();

    FFixedStepClockSnapshot captured_after_start{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(captured_after_start));
    EXPECT_TRUE(SameFixedStepSnapshot(restored_before_start, captured_after_start));
    game.ShutdownForTest();
}

ACS_TEST(GameFixedStepIntegration, AppliesTimeScaleAndSupportsExplicitDisable)
{
    AGameFixedStepProbe game;
    game.SetFixedTimestep(0.1f, 4u);
    game.SetTimeScale(0.5f);
    game.StartForTest();
    AGameFixedStepProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    game.UpdateForTest(0.2f);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_NEAR(scene->LastUpdateDeltaSeconds(), 0.1f, 1.0e-6f);

    game.DisableFixedTimestep();
    EXPECT_FALSE(game.IsFixedTimestepEnabled());
    FFixedStepClockSnapshot disabled_snapshot{};
    EXPECT_FALSE(game.TryCaptureFixedStepSnapshot(disabled_snapshot));
    game.UpdateForTest(1.0f);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->UpdateCount(), 2u);
    game.ShutdownForTest();
}

ACS_TEST(GameFixedStepIntegration, RejectsInvalidConfigurationWithoutChangingState)
{
    AGameFixedStepProbe game;
    FFixedStepClockSnapshot before{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(before));

    FFixedStepOptions invalid_options = before.options;
    invalid_options.maximum_steps_per_advance = 0u;
    EXPECT_FALSE(game.TrySetFixedTimestep(invalid_options));

    FFixedStepClockSnapshot after{};
    EXPECT_TRUE(game.TryCaptureFixedStepSnapshot(after));
    EXPECT_TRUE(SameFixedStepSnapshot(before, after));
}
