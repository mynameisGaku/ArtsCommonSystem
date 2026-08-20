// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedTickInputSource.h"
#include "gameframework/Game.h"
#include "gameframework/InputFrameSource.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/LegacyScene3DAdapter.h"
#include "gameframework/Scene.h"
#include "subsystem/SubsystemOwner.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 固定tick入力を記録する3D操作想定の検証scene。 */
class AFixedRuntimeInputScene final : public AScene {
public:
    /** 入力サービスだけを要求する。 */
    ESvc WantedServices() const noexcept override
    {
        return ESvc::Input;
    }

    /** 前進操作をWへ割り当てる。 */
    void OnEnter() noexcept override
    {
        Services().Input().BindKey(FActionId("MoveForward"), EKey::W);
    }

    /** 現在tickの前進操作を記録する。 */
    void OnFixedUpdate(f32) noexcept override
    {
        const FInputActionState state = Services().Input().Evaluate(FActionId("MoveForward"), Services().FixedInput());
        ++m_FixedUpdateCount;
        if (state.pressed) ++m_PressedCount;
        if (state.held) ++m_HeldCount;
        if (state.released) ++m_ReleasedCount;
    }

    u32 FixedUpdateCount() const noexcept
    {
        return m_FixedUpdateCount;
    }
    u32 PressedCount() const noexcept
    {
        return m_PressedCount;
    }
    u32 HeldCount() const noexcept
    {
        return m_HeldCount;
    }
    u32 ReleasedCount() const noexcept
    {
        return m_ReleasedCount;
    }

private:
    u32 m_FixedUpdateCount = 0u;
    u32 m_PressedCount = 0u;
    u32 m_HeldCount = 0u;
    u32 m_ReleasedCount = 0u;
};

/** scene epoch切替に使う入力なしのoverlay。 */
class AFixedRuntimeOverlayScene final : public AScene {};

/** GPUを起動せずCGameの固定更新を駆動する検証game。 */
class CFixedRuntimeInputGame final : public CGame {
public:
    /** Engine親scopeを準備して初期sceneを開始する。 */
    bool StartForTest() noexcept
    {
        if (!EngineSubsystems().TryInitialize(ESubsystemScope::Engine, nullptr, FSubsystemOwner{this, ESubsystemOwnerKind::Application})) return false;
        OnStart();
        return m_Scene != nullptr && !Scenes().IsEmpty();
    }

    /** 指定秒で一フレーム進める。 */
    void UpdateForTest(f32 delta_seconds) noexcept
    {
        OnUpdate(delta_seconds);
    }

    /** sceneとsubsystemを順に終了する。 */
    void ShutdownForTest() noexcept
    {
        OnShutdown();
        EngineSubsystems().Deinitialize();
    }

    /** scene managerが所有する検証sceneを返す。 */
    AFixedRuntimeInputScene* SceneForTest() const noexcept
    {
        return m_Scene;
    }

protected:
    /** 固定入力を記録する初期sceneを生成する。 */
    TUniquePtr<AScene> InitialScene() noexcept override
    {
        TUniquePtr<AFixedRuntimeInputScene> scene = MakeUnique<AFixedRuntimeInputScene>();
        m_Scene = scene.Get();
        return TUniquePtr<AScene>(Move(scene));
    }

private:
    /** scene managerが所有する検証sceneへの非所有参照。 */
    AFixedRuntimeInputScene* m_Scene = nullptr;
};

/** GPUを起動せずLegacy 3D自由cameraの固定更新を駆動する検証game。 */
class CLegacyFixedOrbitCameraGame final : public CGame {
public:
    /** Engine親scopeを準備してLegacy 3D sceneを開始する。 */
    bool StartForTest() noexcept
    {
        if (!EngineSubsystems().TryInitialize(ESubsystemScope::Engine, nullptr, FSubsystemOwner{this, ESubsystemOwnerKind::Application})) return false;
        OnStart();
        return m_Scene != nullptr && !Scenes().IsEmpty();
    }

    /** 指定秒で一フレーム進める。 */
    void UpdateForTest(f32 delta_seconds) noexcept
    {
        OnUpdate(delta_seconds);
    }

    /** sceneとsubsystemを順に終了する。 */
    void ShutdownForTest() noexcept
    {
        OnShutdown();
        EngineSubsystems().Deinitialize();
    }

    /** scene managerが所有するLegacy 3D sceneを返す。 */
    ALegacyScene3DAdapter* SceneForTest() const noexcept
    {
        return m_Scene;
    }

protected:
    /** Legacy 3D adapterを初期sceneとして生成する。 */
    TUniquePtr<AScene> InitialScene() noexcept override
    {
        /** scene managerへ所有権を渡すLegacy 3D scene。 */
        TUniquePtr<ALegacyScene3DAdapter> scene = MakeUnique<ALegacyScene3DAdapter>();
        m_Scene = scene.Get();
        return TUniquePtr<AScene>(Move(scene));
    }

private:
    /** scene managerが所有するLegacy 3D sceneへの非所有参照。 */
    ALegacyScene3DAdapter* m_Scene = nullptr;
};

/** 物理フレームごとに差し替え可能な入力source。 */
class CScriptedFrameInputSource final : public IInputFrameSource {
public:
    /** Wの状態を次回取得値へ設定する。 */
    bool TrySetForward(bool down, bool pressed, bool released) noexcept
    {
        m_Input.Clear();
        return m_Input.TrySetKeyState(EKey::W, down, pressed, released);
    }

    /** 現在のscripted入力を複製する。 */
    bool TryCaptureFrameInput(FInputStateSnapshot& output) noexcept override
    {
        ++m_CaptureCount;
        output = m_Input;
        return true;
    }

    u32 CaptureCount() const noexcept
    {
        return m_CaptureCount;
    }

private:
    FInputStateSnapshot m_Input;
    u32 m_CaptureCount = 0u;
};

/** tick番号だけから前進操作を再現し、要求順を記録するsource。 */
class CScriptedFixedTickInputSource final : public IFixedTickInputSource {
public:
    /** tick 0で押下、tick 1まで保持、tick 2で解放する。 */
    bool TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output) noexcept override
    {
        if (m_CaptureCount < kMaximumObservedTicks) m_ObservedTicks[m_CaptureCount] = fixed_tick;
        ++m_CaptureCount;
        FInputStateSnapshot staged;
        const bool down = fixed_tick <= 1u;
        if (!staged.TrySetKeyState(EKey::W, down, fixed_tick == 0u, fixed_tick == 2u)) return false;
        output = staged;
        return true;
    }

    u32 CaptureCount() const noexcept
    {
        return m_CaptureCount;
    }
    u64 ObservedTick(u32 index) const noexcept
    {
        return index < kMaximumObservedTicks ? m_ObservedTicks[index] : ~u64{0};
    }

private:
    static constexpr u32 kMaximumObservedTicks = 16u;
    u64 m_ObservedTicks[kMaximumObservedTicks]{};
    u32 m_CaptureCount = 0u;
};

/** 二つの固定時計保存値が全項目で一致する場合はtrueを返す。 */
bool SameClockSnapshot(const timing::FFixedStepClockSnapshot& first,
                       const timing::FFixedStepClockSnapshot& second) noexcept
{
    return first.options.step_seconds == second.options.step_seconds &&
           first.options.maximum_steps_per_advance == second.options.maximum_steps_per_advance &&
           first.options.maximum_accumulated_seconds == second.options.maximum_accumulated_seconds &&
           first.accumulated_seconds == second.accumulated_seconds &&
           first.total_dropped_seconds == second.total_dropped_seconds &&
           first.total_step_count == second.total_step_count;
}

} // namespace

ACS_TEST(GameFixedRuntimeInput, FrameEdgesSurviveUntilThreeDimensionalSimulationTick)
{
    CScriptedFrameInputSource source;
    EXPECT_TRUE(source.TrySetForward(true, true, false));
    CFixedRuntimeInputGame game;
    game.SetFixedTimestep(0.1f, 4u);
    game.SetFixedStepInputSource(source);
    EXPECT_TRUE(game.StartForTest());

    game.UpdateForTest(0.03f);
    EXPECT_TRUE(source.TrySetForward(false, false, true));
    game.UpdateForTest(0.03f);
    EXPECT_TRUE(source.TrySetForward(false, false, false));
    game.UpdateForTest(0.05f);

    AFixedRuntimeInputScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);
    EXPECT_EQ(scene->FixedUpdateCount(), 1u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 0u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);
    EXPECT_EQ(source.CaptureCount(), 3u);
    game.ShutdownForTest();
}

ACS_TEST(GameFixedRuntimeInput, CatchUpRequestsEachFixedTickInOrder)
{
    CScriptedFixedTickInputSource source;
    CFixedRuntimeInputGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedTickInputSource(source);
    EXPECT_TRUE(game.StartForTest());
    game.UpdateForTest(0.375f);

    AFixedRuntimeInputScene* scene = game.SceneForTest();
    EXPECT_TRUE(scene != nullptr);
    EXPECT_EQ(source.CaptureCount(), 3u);
    EXPECT_EQ(source.ObservedTick(0u), 0u);
    EXPECT_EQ(source.ObservedTick(1u), 1u);
    EXPECT_EQ(source.ObservedTick(2u), 2u);
    EXPECT_EQ(scene->FixedUpdateCount(), 3u);
    EXPECT_EQ(scene->PressedCount(), 1u);
    EXPECT_EQ(scene->HeldCount(), 2u);
    EXPECT_EQ(scene->ReleasedCount(), 1u);
    game.ShutdownForTest();
}

ACS_TEST(GameFixedRuntimeInput, LegacyOrbitCameraIgnoresRenderFramePartition)
{
    /** 一つの描画frameで二つの固定tickを供給する入力source。 */
    CScriptedFixedTickInputSource single_frame_source;
    /** 分割描画frameで同じ二つの固定tickを供給する入力source。 */
    CScriptedFixedTickInputSource partitioned_frame_source;
    /** 二つの固定tickを一回の可変更新で進めるgame。 */
    CLegacyFixedOrbitCameraGame single_frame_game;
    /** 同じ二つの固定tickを二回の可変更新へ分けるgame。 */
    CLegacyFixedOrbitCameraGame partitioned_frame_game;
    single_frame_game.SetFixedTimestep(0.125f, 4u);
    partitioned_frame_game.SetFixedTimestep(0.125f, 4u);
    single_frame_game.SetFixedTickInputSource(single_frame_source);
    partitioned_frame_game.SetFixedTickInputSource(partitioned_frame_source);
    EXPECT_TRUE(single_frame_game.StartForTest());
    EXPECT_TRUE(partitioned_frame_game.StartForTest());

    /** 単一frame側のscene manager所有scene。 */
    ALegacyScene3DAdapter* single_frame_scene = single_frame_game.SceneForTest();
    /** 分割frame側のscene manager所有scene。 */
    ALegacyScene3DAdapter* partitioned_frame_scene = partitioned_frame_game.SceneForTest();
    EXPECT_TRUE(single_frame_scene != nullptr);
    EXPECT_TRUE(partitioned_frame_scene != nullptr);
    if (single_frame_scene == nullptr || partitioned_frame_scene == nullptr) {
        partitioned_frame_game.ShutdownForTest();
        single_frame_game.ShutdownForTest();
        return;
    }
    EXPECT_EQ(single_frame_scene->Services().Input().BindingCount(), 6u);
    EXPECT_EQ(partitioned_frame_scene->Services().Input().BindingCount(), 6u);
    /** 固定tick入力を適用する前のcamera位置。 */
    const FVec3 before = single_frame_scene->Camera().Eye();

    single_frame_game.UpdateForTest(0.25f);
    partitioned_frame_game.UpdateForTest(0.05f);
    partitioned_frame_game.UpdateForTest(0.20f);

    /** 単一frame側で二つの固定tickを適用したcamera位置。 */
    const FVec3 single_frame_eye = single_frame_scene->Camera().Eye();
    /** 分割frame側で同じ二つの固定tickを適用したcamera位置。 */
    const FVec3 partitioned_frame_eye = partitioned_frame_scene->Camera().Eye();
    EXPECT_TRUE(single_frame_eye.z > before.z + 1.0f);
    EXPECT_NEAR(single_frame_eye.x, partitioned_frame_eye.x, 1.0e-5f);
    EXPECT_NEAR(single_frame_eye.y, partitioned_frame_eye.y, 1.0e-5f);
    EXPECT_NEAR(single_frame_eye.z, partitioned_frame_eye.z, 1.0e-5f);
    EXPECT_EQ(single_frame_source.CaptureCount(), 2u);
    EXPECT_EQ(partitioned_frame_source.CaptureCount(), 2u);
    partitioned_frame_game.ShutdownForTest();
    single_frame_game.ShutdownForTest();
}

ACS_TEST(GameFixedRuntimeInput, RuntimeRestoreReissuesTheSameDeterministicTick)
{
    CScriptedFixedTickInputSource source;
    CFixedRuntimeInputGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedTickInputSource(source);
    EXPECT_TRUE(game.StartForTest());

    game.UpdateForTest(0.25f);
    FFixedStepRuntimeSnapshot saved;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(saved));
    game.UpdateForTest(0.125f);
    EXPECT_TRUE(game.TryRestoreFixedStepRuntimeSnapshot(saved));
    game.UpdateForTest(0.125f);

    EXPECT_EQ(source.CaptureCount(), 4u);
    EXPECT_EQ(source.ObservedTick(0u), 0u);
    EXPECT_EQ(source.ObservedTick(1u), 1u);
    EXPECT_EQ(source.ObservedTick(2u), 2u);
    EXPECT_EQ(source.ObservedTick(3u), 2u);
    game.ShutdownForTest();
}

ACS_TEST(GameFixedRuntimeInput, RuntimeSnapshotRejectsDifferentGameTransactionally)
{
    CFixedRuntimeInputGame source;
    CFixedRuntimeInputGame target;
    source.SetFixedTimestep(0.125f, 4u);
    target.SetFixedTimestep(0.125f, 4u);
    EXPECT_TRUE(source.StartForTest());
    EXPECT_TRUE(target.StartForTest());

    FFixedStepRuntimeSnapshot source_snapshot;
    FFixedStepRuntimeSnapshot target_before;
    EXPECT_TRUE(source.TryCaptureFixedStepRuntimeSnapshot(source_snapshot));
    EXPECT_TRUE(target.TryCaptureFixedStepRuntimeSnapshot(target_before));
    EXPECT_NE(source_snapshot.runtime_owner_token, target_before.runtime_owner_token);
    EXPECT_FALSE(target.TryRestoreFixedStepRuntimeSnapshot(source_snapshot));

    FFixedStepRuntimeSnapshot target_after;
    EXPECT_TRUE(target.TryCaptureFixedStepRuntimeSnapshot(target_after));
    EXPECT_TRUE(SameClockSnapshot(target_before.clock, target_after.clock));
    EXPECT_EQ(target_before.runtime_owner_token, target_after.runtime_owner_token);
    target.ShutdownForTest();
    source.ShutdownForTest();
}

ACS_TEST(GameFixedRuntimeInput, RuntimeSnapshotRejectsSceneAndSourceChanges)
{
    CScriptedFrameInputSource first_source;
    CScriptedFrameInputSource second_source;
    EXPECT_TRUE(first_source.TrySetForward(true, true, false));
    CFixedRuntimeInputGame game;
    game.SetFixedTimestep(0.125f, 4u);
    game.SetFixedStepInputSource(first_source);
    EXPECT_TRUE(game.StartForTest());
    game.UpdateForTest(0.0625f);

    FFixedStepRuntimeSnapshot source_saved;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(source_saved));
    game.SetFixedStepInputSource(second_source);
    EXPECT_FALSE(game.TryRestoreFixedStepRuntimeSnapshot(source_saved));

    FFixedStepRuntimeSnapshot scene_saved;
    EXPECT_TRUE(game.TryCaptureFixedStepRuntimeSnapshot(scene_saved));
    game.Scenes().PushScene(MakeUnique<AFixedRuntimeOverlayScene>());
    game.UpdateForTest(0.0f);
    EXPECT_NE(scene_saved.active_scene_epoch, game.Scenes().ActiveSceneEpoch());
    EXPECT_FALSE(game.TryRestoreFixedStepRuntimeSnapshot(scene_saved));
    game.ShutdownForTest();
}
