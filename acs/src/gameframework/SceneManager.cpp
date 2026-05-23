// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneManager 実装 (Phase 1 着手)
#include "gameframework/SceneManager.h"
#include "gameframework/Scene.h"
#include "gameframework/Game.h"
#include "gameframework/RenderContext.h"

#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

void SceneManager::ChangeScene(UniquePtr<Scene> next) noexcept {
    if (!next) {
        ACS_LOG_WARN("SceneManager::ChangeScene(nullptr) ignored");
        return;
    }
    _pending_op  = Op::Change;
    _pending_arg = Move(next);
}

void SceneManager::PushScene(UniquePtr<Scene> next) noexcept {
    if (!next) {
        ACS_LOG_WARN("SceneManager::PushScene(nullptr) ignored");
        return;
    }
    _pending_op  = Op::Push;
    _pending_arg = Move(next);
}

void SceneManager::PopScene() noexcept {
    _pending_op = Op::Pop;
    _pending_arg.Reset();
}

Scene* SceneManager::Top() const noexcept {
    if (_stack.IsEmpty()) return nullptr;
    return _stack.Back().Get();
}

u32 SceneManager::Depth() const noexcept {
    return static_cast<u32>(_stack.Size());
}

void SceneManager::DoPushInternal(Game& game, UniquePtr<Scene> next,
                                   bool pause_current) noexcept {
    if (!next) return;
    // 旧 top を OnPause (Push 時のみ。Change は新 top 直入れなので skip)
    if (pause_current && !_stack.IsEmpty()) {
        _stack.Back()->OnPause();
    }
    next->_SetContext(&game, this);
    // Phase 8: WantedServices に基づいて SceneServices を構築・attach。
    // None なら空の SceneServices だが、Services() を呼ばない限りコスト 0。
    const ESvc wanted = next->WantedServices();
    if (wanted != ESvc::None) {
        next->_AttachServices(MakeUnique<SceneServices>(wanted));
    }
    _stack.PushBack(Move(next));
    _stack.Back()->OnEnter();
}

void SceneManager::DoPopInternal(bool resume_new) noexcept {
    if (_stack.IsEmpty()) return;
    _stack.Back()->OnExit();
    // 退場 Scene を ring buffer の現在ヘッドに格納 (3 フレーム保持)。
    // ヘッドは _ApplyPending の冒頭で前進 + 古いスロット解放済。
    _retired[_retire_head] = Move(_stack.Back());
    _stack.PopBack();
    // 新 top を OnResume (Pop 時のみ。Change は次の Push が走るので skip)
    if (resume_new && !_stack.IsEmpty()) {
        _stack.Back()->OnResume();
    }
}

void SceneManager::_ApplyPending(Game& game) noexcept {
    // GPU N+1 frame 遅延削除: ring を 1 つ前進し、新ヘッド位置のスロットを
    // 解放する (= 3 フレーム前に退場した Scene を今ここで破棄)。フレーム
    // インフライト 2 + 1 で「直前 2 フレームを GPU が参照中でも安全」を保つ。
    _retire_head = (_retire_head + 1u) % kRetireRingSize;
    _retired[_retire_head].Reset();

    Op op = _pending_op;
    _pending_op = Op::None;
    UniquePtr<Scene> arg = Move(_pending_arg);

    switch (op) {
    case Op::None:
        return;
    case Op::Change:
        // Pop + Push の合成 = 旧 top を OnExit、新 top を OnEnter。途中で
        // OnResume/OnPause が一瞬走らないように両方 false を渡す。
        DoPopInternal(/*resume_new=*/false);
        DoPushInternal(game, Move(arg), /*pause_current=*/false);
        break;
    case Op::Push:
        // 新 top が乗るので、旧 top を OnPause する。
        DoPushInternal(game, Move(arg), /*pause_current=*/true);
        break;
    case Op::Pop:
        if (_stack.Size() <= 1) {
            ACS_LOG_WARN("SceneManager::PopScene on a stack of size %u (need >=2) — ignored",
                         static_cast<u32>(_stack.Size()));
            return;
        }
        // 新しく top に戻るシーンを OnResume する。
        DoPopInternal(/*resume_new=*/true);
        break;
    }
}

void SceneManager::_Update(f32 dt) noexcept {
    Scene* top = Top();
    if (!top) return;
    // Phase 8: services 2 phase tick — PreUpdate (Clock 進行) → scene OnUpdate
    // → PostUpdate (Tweens/Sequences tick)。Clock 未要求なら raw dt をそのまま OnUpdate へ。
    SceneServices* svc = top->_ServicesOrNull();
    if (svc != nullptr) {
        svc->_PreUpdate(dt);
        const f32 scaled = svc->_ScaledDt(dt);
        top->OnUpdate(scaled);
        svc->_PostUpdate(scaled);
    } else {
        top->OnUpdate(dt);
    }
}

void SceneManager::_FixedUpdate(f32 fixed_dt) noexcept {
    Scene* top = Top();
    if (!top) return;
    top->OnFixedUpdate(fixed_dt);
}

void SceneManager::_Render(RenderContext& rc) noexcept {
    Scene* top = Top();
    if (!top) return;
    top->OnRender(rc);
}

void SceneManager::_DispatchEvent(const Event& e) noexcept {
    Scene* top = Top();
    if (!top) return;
    top->OnEvent(e);
}

void SceneManager::_ShutdownAll() noexcept {
    // top から順に OnExit を呼んでから破棄。
    while (!_stack.IsEmpty()) {
        _stack.Back()->OnExit();
        _stack.PopBack();
    }
    for (u32 i = 0; i < kRetireRingSize; ++i) {
        _retired[i].Reset();
    }
    _pending_arg.Reset();
    _pending_op = Op::None;
}

} // namespace acs::game
