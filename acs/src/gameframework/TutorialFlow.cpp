// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FTutorialFlow 実装 (Phase 2)
//
// 設計メモ:
//   ・step.id と action_id の比較は <cstring> 不使用方針に従い、Pillar T
//     FPartySystem.cpp と同形の自前 StrEq を内部 helper で実装。
//   ・Tick は Phase 2 では state hook のみで内部 timer 不要 (dt 引数は将来
//     拡張のためのプレースホルダ)。dt は参照しないが API として保持。
//   ・AdvanceStep / Skip / Reset すべて冪等で、inactive 状態から呼ばれても
//     副作用が出ないように防御。
#include "gameframework/TutorialFlow.h"

namespace acs::game {

namespace {

// const char* の安全比較 (FPartySystem / FEntitlement と同形)。どちらかが nullptr
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

void FTutorialFlow::AddStep(const FTutorialStep& step) noexcept {
    m_Steps.PushBack(step);
}

void FTutorialFlow::Start() noexcept {
    m_CurrentStep = 0;
    m_Completed    = false;
    if (m_Steps.Size() == 0) {
        // 空チュートリアル: 即完了扱いで終わる (no-op 防御)
        m_Active    = false;
        m_Completed = true;
        return;
    }
    m_Active = true;
}

void FTutorialFlow::AdvanceStep() noexcept {
    if (!m_Active) return;
    ++m_CurrentStep;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) {
        // 最終ステップを越えた → 完了
        m_Active    = false;
        m_Completed = true;
    }
}

void FTutorialFlow::NotifyAction(const char* action_id) noexcept {
    if (!m_Active || action_id == nullptr) return;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) return;
    const FTutorialStep& step = m_Steps[m_CurrentStep];
    if (!step.require_user_action) return;     // 自動 advance しないステップ
    if (StrEq(step.id, action_id)) {
        AdvanceStep();
    }
}

void FTutorialFlow::Reset() noexcept {
    m_CurrentStep = 0;
    m_Active       = false;
    m_Completed    = false;
}

void FTutorialFlow::Skip() noexcept {
    if (m_Completed) return;     // 冪等: 2 度目は no-op
    m_Active    = false;
    m_Completed = true;
}

const FTutorialStep* FTutorialFlow::CurrentStep() const noexcept {
    if (!m_Active) return nullptr;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) return nullptr;
    return &m_Steps[m_CurrentStep];
}

u32 FTutorialFlow::StepCount() const noexcept {
    return static_cast<u32>(m_Steps.Size());
}

void FTutorialFlow::Tick(f32 /*dt*/) noexcept {
    // Phase 2: 内部 timer 無し (require_user_action のステップは NotifyAction
    // または AdvanceStep のみで進む)。将来 hint 出現遅延 / auto-advance step
    // を入れる際にここで m_CurrentStep の elapsed を進める想定。
}

} // namespace acs::game
