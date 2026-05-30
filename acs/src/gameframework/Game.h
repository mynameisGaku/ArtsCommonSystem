// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FGame クラス (Phase 1 着手 / Phase 2 拡張)
//
// FApplication を継承し、FSceneManager を駆動する基底。利用者は派生クラスで
// InitialScene() を override して最初の Scene を返すだけでよい。
//
// 使い方:
//   class MyGame : public acs::game::FGame {
//   protected:
//       acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override {
//           return acs::MakeUnique<TitleScene>();
//       }
//   };
//   ACS_GAME_MAIN(MyGame)
//
// Phase 1: FSceneManager 駆動 + RenderContext 配線。
// Phase 2 (本フェーズ): 固定タイムステップ accumulator + AppState 型消去
//   永続状態 + Scene への dt は time_scale 乗算済を渡す。
//   OnPause/OnResume は FSceneManager 側で配線済 (Push/Pop 時)。
#pragma once

#include "app/Application.h"
#include "memory/UniquePtr.h"
#include "foundation/Move.h"
#include "render/Font.h"
#include "gameframework/SceneManager.h"
#include "gameframework/RenderContext.h"
#include "gameframework/AppState.h"

namespace acs::game {

class Scene;

class FGame : public FApplication {
public:
    FGame() noexcept = default;
    ~FGame() noexcept override = default;

    FGame(const FGame&)            = delete;
    FGame& operator=(const FGame&) = delete;

    FSceneManager&  Scenes()        noexcept { return m_Scenes; }
    RenderContext& GetRenderCtx()  noexcept { return m_RenderCtx; }

    // 時間スケール。Scene::OnUpdate / OnFixedUpdate に渡る dt に乗算される。
    void SetTimeScale(f32 s) noexcept { m_TimeScale = s < 0.0f ? 0.0f : s; }
    f32  TimeScale() const noexcept { return m_TimeScale; }

    // Phase 2 固定タイムステップ設定。
    //   fixed_dt: 固定 step の長さ (秒、典型 1/60 = 0.01667)。0 以下にすると無効化。
    //   max_steps_per_frame: 1 フレームで進める最大 step 数 (暴走防止クランプ)。
    // 既定は fixed_dt=1/60, max=8 (= 0.133s ぶんまでキャッチアップ、それ以上は遅延吸収)。
    void SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame = 8) noexcept {
        m_FixedDt = fixed_dt;
        m_MaxFixedSteps = max_steps_per_frame;
    }
    f32 FixedTimestep() const noexcept { return m_FixedDt; }

    // Phase 2 AppState — シーン跨ぎ永続状態 (型消去、1 個固定)。
    //   `EmplaceAppState<T>(args...)` で構築、`AppState<T>()` で取り出し
    //   (未設定/型不一致は nullptr)。詳細は AppState.h。
    template<typename T, typename... Args>
    T& EmplaceAppState(Args&&... args) noexcept {
        return m_AppState.Emplace<T>(Forward<Args>(args)...);
    }
    template<typename T>
    T* AppState() noexcept { return m_AppState.Get<T>(); }

protected:
    // 派生クラス実装必須: 最初に push される Scene を返す。
    virtual TUniquePtr<Scene> InitialScene() noexcept = 0;

    // FApplication フックを上書きして FSceneManager に流す。
    // 派生がさらに override したい場合は基底を呼ぶこと。
    void OnStart()    noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const Event& e) noexcept override;

private:
    void EnsureUiFont() noexcept;   // 初回 OnRender で default UI フォントを遅延ロード

    FSceneManager  m_Scenes;
    RenderContext m_RenderCtx;
    FAppStateSlot  m_AppState;
    Font          m_UiFont;             // 全シーン共有の HUD フォント (game 寿命)
    bool          m_UiFontReady = false;
    bool          m_UiFontTried = false;
    f32           m_TimeScale       = 1.0f;
    f32           m_FixedDt         = 1.0f / 60.0f;
    f32           m_FixedAccum      = 0.0f;
    u32           m_MaxFixedSteps  = 8;
};

} // namespace acs::game
