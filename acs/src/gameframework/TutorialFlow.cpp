// SPDX-License-Identifier: Apache-2.0
#include "gameframework/TutorialFlow.h"

namespace acs::game {

namespace {

/** 2 つの文字列を比較する。どちらかが nullptr の場合は false を返す。 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

} // namespace

/** 指定ステップを末尾へ追加する。 */
void CTutorialFlow::AddStep(const FTutorialStep& step) noexcept {
    m_Steps.Add(step);
}

/** 先頭から開始し、空フローは即座に完了させる。 */
void CTutorialFlow::Start() noexcept {
    m_CurrentStep = 0;
    m_Completed    = false;
    if (m_Steps.Num() == 0) {
        m_Active    = false;
        m_Completed = true;
        return;
    }
    m_Active = true;
}

/** 進行中なら次へ進み、末尾を越えた場合は完了させる。 */
void CTutorialFlow::AdvanceStep() noexcept {
    if (!m_Active) return;
    ++m_CurrentStep;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Num())) {
        m_Active    = false;
        m_Completed = true;
    }
}

/** 現在ステップが待つ操作と一致した場合だけ次へ進める。 */
void CTutorialFlow::NotifyAction(const char* action_id) noexcept {
    if (!m_Active || action_id == nullptr) return;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Num())) return;
    const FTutorialStep& step = m_Steps[m_CurrentStep];
    if (!step.require_user_action) return;
    if (StrEq(step.id, action_id)) {
        AdvanceStep();
    }
}

/** 登録済みステップを保ったまま開始前の状態へ戻す。 */
void CTutorialFlow::Reset() noexcept {
    m_CurrentStep = 0;
    m_Active       = false;
    m_Completed    = false;
}

/** 未完了ならスキップして完了状態へ移す。 */
void CTutorialFlow::Skip() noexcept {
    if (m_Completed) return;
    m_Active    = false;
    m_Completed = true;
}

/** 進行中のステップを返し、無効な状態では nullptr を返す。 */
const FTutorialStep* CTutorialFlow::CurrentStep() const noexcept {
    if (!m_Active) return nullptr;
    if (m_CurrentStep >= static_cast<u32>(m_Steps.Num())) return nullptr;
    return &m_Steps[m_CurrentStep];
}

/** 登録済みステップ数を返す。 */
u32 CTutorialFlow::StepCount() const noexcept {
    return static_cast<u32>(m_Steps.Num());
}

/** 時間経過による自動遷移を行わない更新口を維持する。 */
void CTutorialFlow::Tick(f32 /*dt*/) noexcept {
}

} // namespace acs::game
