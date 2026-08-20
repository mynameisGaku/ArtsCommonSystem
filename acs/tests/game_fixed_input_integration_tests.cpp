// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Game.h"
#include "gameframework/InputFrameSource.h"
#include "gameframework/Scene.h"
#include "platform/Input.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

namespace {

/** platform 入力へキーの押下または解放を通知する。 */
void SendKeyEvent(EKey key, bool down) noexcept
{
    FEvent event{};
    event.type = down ? EEventType::KeyPressed : EEventType::KeyReleased;
    event.key.key = key;
    event.key.repeat = false;
    FInput::OnEvent(event);
}

/** 他テストや直前フレームの入力状態を安全な無入力へ戻す。 */
void ResetPlatformInput() noexcept
{
    FEvent lost_focus{};
    lost_focus.type = EEventType::WindowLostFocus;
    FInput::OnEvent(lost_focus);
    FInput::Update();
}

/** test が指定した入力 snapshot を一フレームずつ返す明示入力ソース。 */
class FScriptedInputFrameSource final : public IInputFrameSource {
public:
    /** 現在の入力を返し、呼び出し回数を記録する。 */
    bool TryCaptureFrameInput(FInputStateSnapshot& output) noexcept override
    {
        ++m_CaptureCount;
        if (!m_CaptureEnabled) return false;
        output = m_Input;
        return true;
    }

    /** Space キーの現在状態とエッジを設定する。 */
    bool TrySetSpace(bool down, bool pressed, bool released) noexcept
    {
        return m_Input.TrySetKeyState(EKey::Space, down, pressed, released);
    }

    /** 次回以降の取得を成功または失敗させる。 */
    void SetCaptureEnabled(bool enabled) noexcept
    {
        m_CaptureEnabled = enabled;
    }

    /** 入力を取得しようとした回数を返す。 */
    u32 CaptureCount() const noexcept
    {
        return m_CaptureCount;
    }

private:
    /** 次回取得で返す入力。 */
    FInputStateSnapshot m_Input;

    /** 取得を成功させる場合は true。 */
    bool m_CaptureEnabled = true;

    /** 取得を試みたフレーム数。 */
    u32 m_CaptureCount = 0u;
};

/** 固定 tick ごとの Jump 入力を記録する検証用シーン。 */
class AFixedInputProbeScene final : public FScene {
public:
    /** Input サービスを要求する。 */
    ESvc WantedServices() const noexcept override
    {
        return ESvc::Input;
    }

    /** Jump を Space キーへ割り当てる。 */
    void OnEnter() noexcept override
    {
        Services().Input().BindKey(FActionId("Jump"), EKey::Space);
    }

    /** 現在 tick の明示入力から Jump を評価して記録する。 */
    void OnFixedUpdate(f32) noexcept override
    {
        const FInputActionState state = Services().Input().Evaluate(FActionId("Jump"), Services().FixedInput());
        ++m_FixedUpdateCount;
        if (state.pressed) ++m_PressedCount;
        if (state.held) ++m_HeldCount;
        if (state.released) ++m_ReleasedCount;
    }

    /** 固定更新回数を返す。 */
    u32 FixedUpdateCount() const noexcept
    {
        return m_FixedUpdateCount;
    }

    /** 押下を観測した固定更新回数を返す。 */
    u32 PressedCount() const noexcept
    {
        return m_PressedCount;
    }

    /** 保持を観測した固定更新回数を返す。 */
    u32 HeldCount() const noexcept
    {
        return m_HeldCount;
    }

    /** 解放を観測した固定更新回数を返す。 */
    u32 ReleasedCount() const noexcept
    {
        return m_ReleasedCount;
    }

private:
    /** 固定更新回数。 */
    u32 m_FixedUpdateCount = 0u;

    /** 押下を観測した固定更新回数。 */
    u32 m_PressedCount = 0u;

    /** 保持を観測した固定更新回数。 */
    u32 m_HeldCount = 0u;

    /** 解放を観測した固定更新回数。 */
    u32 m_ReleasedCount = 0u;
};

/** Input サービスを持つが固定入力を記録しない重ね合わせシーン。 */
class AFixedInputOverlayScene final : public FScene {
public:
    /** Input サービスを要求する。 */
    ESvc WantedServices() const noexcept override
    {
        return ESvc::Input;
    }
};

/** GPU を起動せず FGame の固定入力結線を検証するゲーム。 */
class AFixedInputProbeGame final : public FGame {
public:
    /** 初期シーンを取り付ける。 */
    void StartForTest() noexcept
    {
        OnStart();
    }

    /** 指定した経過秒で一フレーム進める。 */
    void UpdateForTest(f32 delta_seconds) noexcept
    {
        OnUpdate(delta_seconds);
    }

    /** シーンとサブシステムを終了する。 */
    void ShutdownForTest() noexcept
    {
        OnShutdown();
    }

    /** 初期シーンへの非所有参照を返す。 */
    AFixedInputProbeScene* SceneForTest() const noexcept
    {
        return m_Scene;
    }

protected:
    /** 固定入力を記録する初期シーンを生成する。 */
    TUniquePtr<FScene> InitialScene() noexcept override
    {
        TUniquePtr<AFixedInputProbeScene> scene = MakeUnique<AFixedInputProbeScene>();
        m_Scene = scene.Get();
        return TUniquePtr<FScene>(Move(scene));
    }

private:
    /** FSceneManager が所有する初期シーンへの非所有参照。 */
    AFixedInputProbeScene* m_Scene = nullptr;
};

/** Input サービスを要求しない snapshot 境界検証用シーン。 */
class ANoInputProbeScene final : public FScene {};

/** Input サービスを持たない active scene への復元拒否を検証するゲーム。 */
class ANoInputProbeGame final : public FGame {
public:
    /** 初期シーンを取り付ける。 */
    void StartForTest() noexcept
    {
        OnStart();
    }

    /** シーンとサブシステムを終了する。 */
    void ShutdownForTest() noexcept
    {
        OnShutdown();
    }

protected:
    /** Input サービスを持たない初期シーンを生成する。 */
    TUniquePtr<FScene> InitialScene() noexcept override
    {
        return MakeUnique<ANoInputProbeScene>();
    }
};

/** 二つの固定時計保存値が全項目で一致する場合は true を返す。 */
bool SameClockSnapshot(const FFixedStepClockSnapshot& first, const FFixedStepClockSnapshot& second) noexcept
{
    return first.options.step_seconds == second.options.step_seconds &&
           first.options.maximum_steps_per_advance == second.options.maximum_steps_per_advance &&
           first.options.maximum_accumulated_seconds == second.options.maximum_accumulated_seconds &&
           first.accumulated_seconds == second.accumulated_seconds &&
           first.total_dropped_seconds == second.total_dropped_seconds &&
           first.total_step_count == second.total_step_count;
}

} // namespace

ACS_TEST(GameFixedInputIntegration, CatchUpConsumesPressedOnlyOnFirstTick)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.3125f);
    EXPECT_EQ(scene->FixedUpdateCount(), 2u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 2u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    FInput::Update();
    game.UpdateForTest(0.0625f);
    EXPECT_EQ(scene->FixedUpdateCount(), 3u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 3u);

    FInput::Update();
    SendKeyEvent(EKey::Space, false);
    game.UpdateForTest(0.125f);
    EXPECT_EQ(scene->FixedUpdateCount(), 4u);
    EXPECT_EQ(scene->HeldCount(), 3u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, ZeroStepFramesPreserveShortTap)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.1f, 4u);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.03f);
    FInput::Update();
    SendKeyEvent(EKey::Space, false);
    game.UpdateForTest(0.03f);
    FInput::Update();
    game.UpdateForTest(0.05f);

    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, ResumeDiscardsPausedScenePendingEdges)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.1f, 4u);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.02f);
    FInput::Update();

    game.Scenes().PushScene(MakeUnique<AFixedInputOverlayScene>());
    game.UpdateForTest(0.02f);
    FInput::Update();
    SendKeyEvent(EKey::Space, false);
    game.UpdateForTest(0.02f);
    FInput::Update();

    game.Scenes().PopScene();
    game.UpdateForTest(0.05f);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 0u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, PausedTimeScaleDoesNotReplayOldEdges)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.1f, 4u);
    game.SetTimeScale(0.0f);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.2f);
    FInput::Update();
    SendKeyEvent(EKey::Space, false);
    game.UpdateForTest(0.2f);
    FInput::Update();

    game.SetTimeScale(1.0f);
    game.UpdateForTest(0.1f);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 0u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, ExplicitSourceFeedsHeadlessFixedUpdatesOncePerFrame)
{
    ResetPlatformInput();
    FScriptedInputFrameSource source;
    EXPECT_TRUE(source.TrySetSpace(true, true, false));

    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedStepInputSource(source);
    EXPECT_FALSE(game.UsesPlatformFixedStepInput());
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    game.UpdateForTest(0.0625f);
    EXPECT_EQ(source.CaptureCount(), 1u);
    EXPECT_EQ(scene->FixedUpdateCount(), 0u);

    EXPECT_TRUE(source.TrySetSpace(true, false, false));
    game.UpdateForTest(0.0625f);
    EXPECT_EQ(source.CaptureCount(), 2u);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 1u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, SourceSwitchDiscardsPendingInputBeforePlatformResume)
{
    ResetPlatformInput();
    FScriptedInputFrameSource source;
    EXPECT_TRUE(source.TrySetSpace(true, true, false));

    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedStepInputSource(source);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    game.UpdateForTest(0.0625f);
    game.ResetFixedStepInputSource();
    EXPECT_TRUE(game.UsesPlatformFixedStepInput());
    game.UpdateForTest(0.0625f);

    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 0u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, FailedExplicitSourceClearsPendingInputTransactionally)
{
    ResetPlatformInput();
    FScriptedInputFrameSource source;
    EXPECT_TRUE(source.TrySetSpace(true, true, false));

    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedStepInputSource(source);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    game.UpdateForTest(0.0625f);
    source.SetCaptureEnabled(false);
    game.UpdateForTest(0.0625f);

    EXPECT_EQ(source.CaptureCount(), 2u);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 0u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 0u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, RuntimeSnapshotRestoresClockAndPendingInputTogether)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.StartForTest();
    AFixedInputProbeScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);

    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.0625f);
    FFixedStepRuntimeSnapshot saved;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(saved));
    EXPECT_TRUE(saved.fixed_step_enabled);
    EXPECT_TRUE(saved.input.has_input_state);
    EXPECT_NEAR(saved.clock.accumulated_seconds, 0.0625, 1.0e-9);

    FInput::Update();
    SendKeyEvent(EKey::Space, false);
    game.UpdateForTest(0.0625f);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);

    EXPECT_TRUE(game.TryRestoreFixedStepRuntimeSnapshot(saved));
    EXPECT_NEAR(game.FixedStepInterpolationAlpha(), 0.5, 1.0e-9);
    ResetPlatformInput();
    SendKeyEvent(EKey::Space, true);
    FInput::Update();
    game.UpdateForTest(0.0625f);
    EXPECT_EQ(scene->FixedUpdateCount(), 2u);
    EXPECT_EQ(scene->PressedCount(), 2u);
    EXPECT_EQ(scene->HeldCount(), 1u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, InvalidRuntimeSnapshotPreservesClockAndInput)
{
    ResetPlatformInput();
    AFixedInputProbeGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.StartForTest();
    SendKeyEvent(EKey::Space, true);
    game.UpdateForTest(0.0625f);

    FFixedStepRuntimeSnapshot before;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(before));
    FFixedStepRuntimeSnapshot invalid = before;
    invalid.clock.options.step_seconds = 0.0;
    EXPECT_FALSE(game.TryRestoreFixedStepRuntimeSnapshot(invalid));

    FFixedStepRuntimeSnapshot after;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(after));
    EXPECT_TRUE(SameClockSnapshot(before.clock, after.clock));
    EXPECT_EQ(before.fixed_step_enabled, after.fixed_step_enabled);
    EXPECT_EQ(before.input.has_input_state, after.input.has_input_state);
    EXPECT_TRUE(after.input.pending_input.IsKeyDown(EKey::Space));
    EXPECT_TRUE(after.input.pending_input.IsKeyPressed(EKey::Space));

    game.ShutdownForTest();
    ResetPlatformInput();
}

ACS_TEST(GameFixedInputIntegration, RuntimeSnapshotRejectsMissingInputServiceTransactionally)
{
    ResetPlatformInput();
    FFixedStepRuntimeSnapshot source_snapshot;
    {
        AFixedInputProbeGame source;
        source.SetFixedTimestep(0.125f, 4u);
        source.StartForTest();
        SendKeyEvent(EKey::Space, true);
        source.UpdateForTest(0.0625f);
        EXPECT_TRUE(source.TryCaptureFixedStepRuntimeSnapshot(source_snapshot));
        EXPECT_TRUE(source_snapshot.input.has_input_state);
        source.ShutdownForTest();
    }
    ResetPlatformInput();

    ANoInputProbeGame target;
    target.SetFixedTimestep(0.25f, 2u);
    target.StartForTest();
    FFixedStepRuntimeSnapshot before;
    EXPECT_TRUE(target.TryCaptureFixedStepRuntimeSnapshot(before));
    EXPECT_FALSE(target.TryRestoreFixedStepRuntimeSnapshot(source_snapshot));
    FFixedStepRuntimeSnapshot after;
    EXPECT_TRUE(target.TryCaptureFixedStepRuntimeSnapshot(after));
    EXPECT_TRUE(SameClockSnapshot(before.clock, after.clock));
    EXPECT_FALSE(after.input.has_input_state);

    target.ShutdownForTest();
    ResetPlatformInput();
}
