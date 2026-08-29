// SPDX-License-Identifier: Apache-2.0
#include "gameframework/RenderFrameSubmissionSubsystem.h"

namespace acs::game {

bool CRenderFrameSubmissionSubsystem::TryBind(
    void* listener, FListener callback) noexcept
{
    if (listener == nullptr || callback == nullptr) return false;
    if (m_Listener != nullptr && m_Listener != listener) return false;
    m_Listener = listener;
    m_Callback = callback;
    return true;
}

void CRenderFrameSubmissionSubsystem::Unbind(void* listener) noexcept
{
    if (m_Listener != listener) return;
    m_Listener = nullptr;
    m_Callback = nullptr;
}

void CRenderFrameSubmissionSubsystem::Publish(
    const FRendererFrameEndResult& result) noexcept
{
    // callback内で購読解除されても、今回の呼び出し先だけは同じ組として保持する。
    void* const listener = m_Listener;
    const FListener callback = m_Callback;
    if (listener != nullptr && callback != nullptr) callback(listener, result);
}

void CRenderFrameSubmissionSubsystem::OnDeinitialize() noexcept
{
    m_Listener = nullptr;
    m_Callback = nullptr;
}

} // namespace acs::game
