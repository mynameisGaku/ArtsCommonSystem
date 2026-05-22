// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — Game クラス (Phase 1 着手 / Phase 2 拡張)
//
// Application を継承し、SceneManager を駆動する基底。利用者は派生クラスで
// InitialScene() を override して最初の Scene を返すだけでよい。
//
// 使い方:
//   class MyGame : public acs::game::Game {
//   protected:
//       acs::UniquePtr<acs::game::Scene> InitialScene() noexcept override {
//           return acs::MakeUnique<TitleScene>();
//       }
//   };
//   ACS_GAME_MAIN(MyGame)
//
// Phase 1: SceneManager 駆動 + RenderContext 配線。
// Phase 2 (本フェーズ): 固定タイムステップ accumulator + AppState 型消去
//   永続状態 + Scene への dt は time_scale 乗算済を渡す。
//   OnPause/OnResume は SceneManager 側で配線済 (Push/Pop 時)。
#pragma once

#include "app/Application.h"
#include "memory/UniquePtr.h"
#include "foundation/Move.h"
#include "gameframework/SceneManager.h"
#include "gameframework/RenderContext.h"
#include "gameframework/AppState.h"

namespace acs::game {

class Scene;

class Game : public Application {
public:
    Game() noexcept = default;
    ~Game() noexcept override = default;

    Game(const Game&)            = delete;
    Game& operator=(const Game&) = delete;

    SceneManager&  Scenes()        noexcept { return _scenes; }
    RenderContext& GetRenderCtx()  noexcept { return _render_ctx; }

    // 時間スケール。Scene::OnUpdate / OnFixedUpdate に渡る dt に乗算される。
    void SetTimeScale(f32 s) noexcept { _time_scale = s < 0.0f ? 0.0f : s; }
    f32  TimeScale() const noexcept { return _time_scale; }

    // Phase 2 固定タイムステップ設定。
    //   fixed_dt: 固定 step の長さ (秒、典型 1/60 = 0.01667)。0 以下にすると無効化。
    //   max_steps_per_frame: 1 フレームで進める最大 step 数 (暴走防止クランプ)。
    // 既定は fixed_dt=1/60, max=8 (= 0.133s ぶんまでキャッチアップ、それ以上は遅延吸収)。
    void SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame = 8) noexcept {
        _fixed_dt = fixed_dt;
        _max_fixed_steps = max_steps_per_frame;
    }
    f32 FixedTimestep() const noexcept { return _fixed_dt; }

    // Phase 2 AppState — シーン跨ぎ永続状態 (型消去、1 個固定)。
    //   `EmplaceAppState<T>(args...)` で構築、`AppState<T>()` で取り出し
    //   (未設定/型不一致は nullptr)。詳細は AppState.h。
    template<typename T, typename... Args>
    T& EmplaceAppState(Args&&... args) noexcept {
        return _app_state.Emplace<T>(Forward<Args>(args)...);
    }
    template<typename T>
    T* AppState() noexcept { return _app_state.Get<T>(); }

protected:
    // 派生クラス実装必須: 最初に push される Scene を返す。
    virtual UniquePtr<Scene> InitialScene() noexcept = 0;

    // Application フックを上書きして SceneManager に流す。
    // 派生がさらに override したい場合は基底を呼ぶこと。
    void OnStart()    noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const Event& e) noexcept override;

private:
    SceneManager  _scenes;
    RenderContext _render_ctx;
    AppStateSlot  _app_state;
    f32           _time_scale       = 1.0f;
    f32           _fixed_dt         = 1.0f / 60.0f;
    f32           _fixed_accum      = 0.0f;
    u32           _max_fixed_steps  = 8;
};

} // namespace acs::game
