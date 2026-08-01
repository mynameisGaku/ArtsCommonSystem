// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CTutorialFlow 実装
//
// 設計メモ:
//   ・step.id と action_id の比較は <cstring> 不使用方針に従い、自前 StrEq を
//     内部 helper で実装。
//   ・Tick は state hook のみで内部 timer 不要 (dt 引数は拡張のための
//     プレースホルダ)。dt は参照しないが API として保持。
//   ・AdvanceStep / Skip / Reset すべて冪等で、inactive 状態から呼ばれても
//     副作用が出ないように防御。
#include "gameframework/TutorialFlow.h"

namespace acs::game {

namespace {

/**
 * const char* の安全な等価比較を行う。
 *
 * @details どちらかが nullptr なら false。終端ヌルまで 1 文字ずつ比較し、両方が同時に '\0' に達したときのみ true。
 * @param a 比較する文字列 1 (nullptr 可)。
 * @param b 比較する文字列 2 (nullptr 可)。
 * @return 内容が完全一致すれば true。
 */
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

void CTutorialFlow::AddStep(const FTutorialStep& step) noexcept {
    m_Steps.PushBack(step);
}

void CTutorialFlow::Start() noexcept {
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

void CTutorialFlow::AdvanceStep() noexcept {
    if (!m_Active) return;
    ++m_CurrentStep;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) {
        // 最終ステップを越えた → 完了
        m_Active    = false;
        m_Completed = true;
    }
}

void CTutorialFlow::NotifyAction(const char* action_id) noexcept {
    if (!m_Active || action_id == nullptr) return;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) return;
    const FTutorialStep& step = m_Steps[m_CurrentStep];
    if (!step.require_user_action) return;     // 自動 advance しないステップ
    if (StrEq(step.id, action_id)) {
        AdvanceStep();
    }
}

void CTutorialFlow::Reset() noexcept {
    m_CurrentStep = 0;
    m_Active       = false;
    m_Completed    = false;
}

void CTutorialFlow::Skip() noexcept {
    if (m_Completed) return;     // 冪等: 2 度目は no-op
    m_Active    = false;
    m_Completed = true;
}

const FTutorialStep* CTutorialFlow::CurrentStep() const noexcept {
    if (!m_Active) return nullptr;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Size())) return nullptr;
    return &m_Steps[m_CurrentStep];
}

u32 CTutorialFlow::StepCount() const noexcept {
    return static_cast<u32>(m_Steps.Size());
}

void CTutorialFlow::Tick(f32 /*dt*/) noexcept {
    // 内部 timer 無し (require_user_action のステップは NotifyAction
    // または AdvanceStep のみで進む)。hint 出現遅延 / auto-advance step
    // を入れる際にここで m_CurrentStep の elapsed を進める想定。
}

} // namespace acs::game
