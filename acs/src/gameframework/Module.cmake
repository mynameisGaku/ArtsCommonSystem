# GameFramework — `acs::Application` の上に Scene 切替・固定タイムステップ等を載せる
# ゲーム制作フレームワーク。v3 確定の 8 ピラー (v13 で 24 ピラー) のうち、
# Phase 1 では Pillar A (App & Scene) のみを実装する。
#
# 利用者の include パス:
#   #include "gameframework/GameFramework.h"   // まとめ
# あるいは個別:
#   #include "gameframework/Scene.h"
#   #include "gameframework/SceneManager.h"
#   #include "gameframework/Game.h"
#   #include "gameframework/RenderContext.h"
#   #include "gameframework/EntryPoint.h"
#
# モジュール命名: ACS::GameFramework
# 名前空間:       acs::game

acs_module(
    NAME    GameFramework
    TYPE    Runtime
    SOURCES
        Game.cpp
        SceneManager.cpp
        Tween.cpp
        Sequence.cpp
        Node2D.cpp
        InputMap.cpp
        SceneServices.cpp
        CollisionWorld2D.cpp
        PhysicsBody2D.cpp
        # ----- Phase 12: parallel agent batch -----
        Random.cpp
        DebugDraw.cpp
        PhotoMode.cpp
        AmbientDirector.cpp
        SpriteAnimator.cpp
        EffectSystem.cpp
        AssetBundle.cpp
        BehaviorTree.cpp
        Entitlement.cpp
        AudioDirector.cpp
        SaveSlot.cpp
        ModRegistry.cpp
        SteamworksBridge.cpp
        BackendClient.cpp
        MlRuntime.cpp
        Settings.cpp
        PartySystem.cpp
        DevConsole.cpp
        # ----- Phase 13: 2 巡目 15 parallel agent batch -----
        FadeTransition.cpp
        SceneEventBus.cpp
        StreamingDirector.cpp
        OpenXrBridge.cpp
        StudioWorkflow.cpp
        Lockstep.cpp
        UiLayer.cpp
        Pathfinding.cpp
        Tilemap.cpp
        WeatherSystem.cpp
        DamageFeedback.cpp
        WorkshopBridge.cpp
        LlmSafetyPipeline.cpp
        TutorialFlow.cpp
        DialogueSystem.cpp
        # ----- Phase 14: 3 巡目 15 parallel agent batch -----
        SceneTimer.cpp
        TriggerWorld2D.cpp
        AssetPack.cpp
        PrefabSystem.cpp
        CameraStack.cpp
        LocalizationDirector.cpp
        AchievementManager.cpp
        DebugOverlay.cpp
        ParticleEffectSystem.cpp
        MusicDirector.cpp
        AccessibilityProfile.cpp
        PrivacyDirector.cpp
        CinematicsDirector.cpp
        InspectorSeam.cpp
        Progression.cpp
        # ----- Phase 15: 4 巡目 15 parallel agent batch -----
        AnimationCurve.cpp
        InputRecorder.cpp
        Perception.cpp
        SeasonPass.cpp
        VoiceChat.cpp
        PauseDirector.cpp
        HotReload.cpp
        PerfBudget.cpp
        SceneCommandQueue.cpp
        SocialModeration.cpp
        SpatialAudio.cpp
        CameraShakePresets.cpp
        ContentModerator.cpp
        DialogueLocalizer.cpp
        # ----- Phase 16: 5 巡目 10 parallel agent batch -----
        WaterVolume.cpp
        CrashReporter.cpp
        TelemetryDirector.cpp
        GameFlow.cpp
        DynamicDifficulty.cpp
        ReplayDirector.cpp
        SpritePack.cpp
        EconomyDirector.cpp
        CharacterCustomizer.cpp
        CombatStateMachine.cpp
        # ----- Phase 17: 6 巡目 10 parallel agent batch (ゲームドメイン runtime) -----
        HealthSystem.cpp
        InventorySystem.cpp
        CooldownTimer.cpp
        BuffSystem.cpp
        ScoreSystem.cpp
        WaveSpawner.cpp
        PickupSystem.cpp
        WeaponSystem.cpp
        ProjectileSystem.cpp
        NodePool.cpp
        # ----- Phase 18: 7 巡目 10 parallel agent batch (ジャンルキット) -----
        DeckSystem.cpp
        BeatGrid.cpp
        DungeonGenerator.cpp
        LapTimer.cpp
        CraftingSystem.cpp
        MatchGrid.cpp
        TurnManager.cpp
        CheckpointSystem.cpp
        HungerSystem.cpp
        DialogueScript.cpp
        # ----- Phase 19b: ParticleEditor (in-game editor、tools/fxedit/) -----
        tools/fxedit/ParticleEditorPanel.cpp
        tools/fxedit/ParticleEditorPreview.cpp
        tools/fxedit/FxeditSerializer.cpp
        # ----- Phase 20: SceneInspector (in-game editor、tools/inspector/) -----
        tools/inspector/HierarchyPanel.cpp
        tools/inspector/InspectorPanel.cpp
        tools/inspector/EditorToolbar.cpp
        tools/inspector/SelectionService.cpp
        # ----- Phase 21a: editor 共通基盤 (tools/editor_core/) -----
        tools/editor_core/EditorPanel.cpp
        tools/editor_core/EditorWorkspace.cpp
        tools/editor_core/UndoStack.cpp
        tools/editor_core/EditorCamera.cpp
        tools/editor_core/AssetBrowser.cpp
        tools/editor_core/PropertyDrawer.cpp
        tools/editor_core/EditorGizmo.cpp
        tools/editor_core/EditorTheme.cpp
    HEADERS
        GameFramework.h
        Game.h
        Scene.h
        SceneManager.h
        RenderContext.h
        AppState.h
        Easing.h
        Clock.h
        Tween.h
        Sequence.h
        StateMachine.h
        Transform2D.h
        Node2D.h
        Component2D.h
        InputMap.h
        Camera2D.h
        CollisionWorld2D.h
        PhysicsBody2D.h
        SceneServices.h
        # ----- Phase 12: parallel agent batch -----
        NodeId.h
        Pool.h
        Random.h
        DebugDraw.h
        PhotoMode.h
        AmbientDirector.h
        SpriteAnimator.h
        EffectSystem.h
        AssetBundle.h
        BehaviorTree.h
        Entitlement.h
        AudioDirector.h
        SaveSlot.h
        ModRegistry.h
        SteamworksBridge.h
        BackendClient.h
        MlRuntime.h
        Settings.h
        PartySystem.h
        DevConsole.h
        # ----- Phase 13: 2 巡目 15 parallel agent batch -----
        FadeTransition.h
        SceneEventBus.h
        StreamingDirector.h
        OpenXrBridge.h
        StudioWorkflow.h
        Lockstep.h
        UiLayer.h
        Pathfinding.h
        Tilemap.h
        WeatherSystem.h
        DamageFeedback.h
        WorkshopBridge.h
        LlmSafetyPipeline.h
        TutorialFlow.h
        DialogueSystem.h
        # ----- Phase 14: 3 巡目 15 parallel agent batch -----
        SceneTimer.h
        TriggerWorld2D.h
        AssetPack.h
        PrefabSystem.h
        CameraStack.h
        LocalizationDirector.h
        AchievementManager.h
        DebugOverlay.h
        ParticleEffectSystem.h
        MusicDirector.h
        AccessibilityProfile.h
        PrivacyDirector.h
        CinematicsDirector.h
        InspectorSeam.h
        Progression.h
        # ----- Phase 15: 4 巡目 15 parallel agent batch -----
        AnimationCurve.h
        InputRecorder.h
        Perception.h
        SeasonPass.h
        VoiceChat.h
        TypeInfo.h
        PauseDirector.h
        HotReload.h
        PerfBudget.h
        SceneCommandQueue.h
        SocialModeration.h
        SpatialAudio.h
        CameraShakePresets.h
        ContentModerator.h
        DialogueLocalizer.h
        # ----- Phase 16: 5 巡目 10 parallel agent batch -----
        WaterVolume.h
        CrashReporter.h
        TelemetryDirector.h
        GameFlow.h
        DynamicDifficulty.h
        ReplayDirector.h
        SpritePack.h
        EconomyDirector.h
        CharacterCustomizer.h
        CombatStateMachine.h
        # ----- Phase 17: 6 巡目 10 parallel agent batch (ゲームドメイン runtime) -----
        HealthSystem.h
        InventorySystem.h
        CooldownTimer.h
        BuffSystem.h
        ScoreSystem.h
        WaveSpawner.h
        PickupSystem.h
        WeaponSystem.h
        ProjectileSystem.h
        NodePool.h
        # ----- Phase 18: 7 巡目 10 parallel agent batch (ジャンルキット) -----
        DeckSystem.h
        BeatGrid.h
        DungeonGenerator.h
        LapTimer.h
        CraftingSystem.h
        MatchGrid.h
        TurnManager.h
        CheckpointSystem.h
        HungerSystem.h
        DialogueScript.h
        # ----- Phase 19b: ParticleEditor (in-game editor、tools/fxedit/) -----
        tools/fxedit/ParticleEditorPanel.h
        tools/fxedit/ParticleEditorPreview.h
        tools/fxedit/FxeditSerializer.h
        # ----- Phase 20: SceneInspector (in-game editor、tools/inspector/) -----
        tools/inspector/HierarchyPanel.h
        tools/inspector/InspectorPanel.h
        tools/inspector/EditorToolbar.h
        tools/inspector/SelectionService.h
        # ----- Phase 21a: editor 共通基盤 (tools/editor_core/) -----
        tools/editor_core/EditorPanel.h
        tools/editor_core/EditorWorkspace.h
        tools/editor_core/EditorCommand.h
        tools/editor_core/UndoStack.h
        tools/editor_core/EditorCamera.h
        tools/editor_core/AssetBrowser.h
        tools/editor_core/PropertyDrawer.h
        tools/editor_core/EditorGizmo.h
        tools/editor_core/EditorTheme.h
        EntryPoint.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
        Platform
        Render
        App
)
