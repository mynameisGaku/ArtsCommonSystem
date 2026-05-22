// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — TutorialFlow 実装 (Phase 2)
//
// 設計メモ:
//   ・step.id と action_id の比較は <cstring> 不使用方針に従い、Pillar T
//     PartySystem.cpp と同形の自前 StrEq を内部 helper で実装。
//   ・Tick は Phase 2 では state hook のみで内部 timer 不要 (dt 引数は将来
//     拡張のためのプレースホルダ)。dt は参照しないが API として保持。
//   ・AdvanceStep / Skip / Reset すべて冪等で、inactive 状態から呼ばれても
//     副作用が出ないように防御。
#include "gameframework/TutorialFlow.h"

namespace acs::game {

namespace {

// const char* の安全比較 (PartySystem / Entitlement と同形)。どちらかが nullptr
// なら false。終端ヌルまで一致比較。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;     // 両方が '\0' のときのみ true
}

} // namespace

void TutorialFlow::AddStep(const TutorialStep& step) noexcept {
    _steps.PushBack(step);
}

void TutorialFlow::Start() noexcept {
    _current_step = 0;
    _completed    = false;
    if (_steps.Size() == 0) {
        // 空チュートリアル: 即完了扱いで終わる (no-op 防御)
        _active    = false;
        _completed = true;
        return;
    }
    _active = true;
}

void TutorialFlow::AdvanceStep() noexcept {
    if (!_active) return;
    ++_current_step;
    if (_current_step >= static_cast<u32>(_steps.Size())) {
        // 最終ステップを越えた → 完了
        _active    = false;
        _completed = true;
    }
}

void TutorialFlow::NotifyAction(const char* action_id) noexcept {
    if (!_active || action_id == nullptr) return;
    if (_current_step >= static_cast<u32>(_steps.Size())) return;
    const TutorialStep& step = _steps[_current_step];
    if (!step.require_user_action) return;     // 自動 advance しないステップ
    if (StrEq(step.id, action_id)) {
        AdvanceStep();
    }
}

void TutorialFlow::Reset() noexcept {
    _current_step = 0;
    _active       = false;
    _completed    = false;
}

void TutorialFlow::Skip() noexcept {
    if (_completed) return;     // 冪等: 2 度目は no-op
    _active    = false;
    _completed = true;
}

const TutorialStep* TutorialFlow::CurrentStep() const noexcept {
    if (!_active) return nullptr;
    if (_current_step >= static_cast<u32>(_steps.Size())) return nullptr;
    return &_steps[_current_step];
}

u32 TutorialFlow::StepCount() const noexcept {
    return static_cast<u32>(_steps.Size());
}

void TutorialFlow::Tick(f32 /*dt*/) noexcept {
    // Phase 2: 内部 timer 無し (require_user_action のステップは NotifyAction
    // または AdvanceStep のみで進む)。将来 hint 出現遅延 / auto-advance step
    // を入れる際にここで _current_step の elapsed を進める想定。
}

} // namespace acs::game
