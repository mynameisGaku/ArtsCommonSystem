// SPDX-License-Identifier: Apache-2.0
#include "app/TimerSubsystem.h"

#include "app/Application.h"
#include "event/TimerManager.h"

namespace acs {

/** Application owner を検証し、既存タイマー管理器を非所有で結び付ける。 */
bool FTimerSubsystem::OnOwnerAssigned() noexcept
{
    m_Timers = nullptr;
    if (OwnerKind() == ESubsystemOwnerKind::Unknown) return true;
    if (OwnerKind() != ESubsystemOwnerKind::Application || Owner() == nullptr) {
        return false;
    }

    // 検証済みの Application owner。
    FApplication* const application = static_cast<FApplication*>(Owner());
    m_Timers = &application->GetTimers();
    return true;
}

/** 終了する Engine スコープから非所有参照を外す。 */
void FTimerSubsystem::OnDeinitialize() noexcept
{
    m_Timers = nullptr;
}

/** PreUpdate で非スケール時間を既存タイマー管理器へ渡す。 */
void FTimerSubsystem::OnTickFrame(const FSubsystemFrameContext& context) noexcept
{
    if (m_Timers == nullptr) {
        return;
    }
    m_Timers->Tick(context.unscaled_delta_seconds);
}

} // namespace acs
