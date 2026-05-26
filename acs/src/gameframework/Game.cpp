// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FGame 実装 (Phase 1 着手)
#include "gameframework/Game.h"
#include "gameframework/Scene.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

void FGame::OnStart() noexcept {
    TUniquePtr<FScene> first = InitialScene();
    if (!first) {
        ACS_LOG_ERROR("FGame::InitialScene() returned null — Quit");
        Quit();
        return;
    }
    _scenes.PushScene(Move(first));
    _scenes._ApplyPending(*this);     // 起動時の最初の遷移は即時適用
}

void FGame::OnUpdate(f32 dt) noexcept {
    const f32 scaled_dt = dt * _time_scale;
    _scenes._ApplyPending(*this);

    // Phase 2 固定タイムステップ accumulator (Phase 1 で宣言だけだった
    // FScene::OnFixedUpdate が実際に呼ばれるようになる)。
    //   accumulator += dt → fixed_dt 単位で消費しつつ OnFixedUpdate を呼ぶ。
    //   max_fixed_steps を超える遅延は捨てる (= spiral of death 防止)。
    //   fixed_dt <= 0 なら固定 update は無効 (旧挙動)。
    if (_fixed_dt > 0.0f) {
        _fixed_accum += scaled_dt;
        u32 steps = 0;
        while (_fixed_accum >= _fixed_dt && steps < _max_fixed_steps) {
            _scenes._FixedUpdate(_fixed_dt);
            _fixed_accum -= _fixed_dt;
            ++steps;
        }
        // 上限超え分は捨てる (遅延を引きずらない)
        if (steps == _max_fixed_steps && _fixed_accum > _fixed_dt) {
            _fixed_accum = 0.0f;
        }
    }

    // variable-rate update (毎フレーム dt)
    _scenes._Update(scaled_dt);
}

void FGame::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    _render_ctx._BeginFrame(GetRenderer(), *cl, sc->Width(), sc->Height());
    _scenes._Render(_render_ctx);
    _render_ctx._EndFrame();
}

void FGame::OnShutdown() noexcept {
    _scenes._ShutdownAll();
}

void FGame::OnEvent(const FEvent& e) noexcept {
    _scenes._DispatchEvent(e);
}

} // namespace acs::game
