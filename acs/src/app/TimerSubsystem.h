// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "subsystem/Subsystem.h"

namespace acs {

class CTimerManager;

/** FApplication が所有するタイマー管理器を Engine スコープへ公開するアダプター。 */
class FTimerSubsystem final : public FSubsystem {
public:
    ACS_SUBSYSTEM_KIND(FTimerSubsystem)

    /** Application owner を検証し、既存タイマー管理器を非所有で結び付ける。 */
    bool OnOwnerAssigned() noexcept override;

    /** 終了する Engine スコープから非所有参照を外す。 */
    void OnDeinitialize() noexcept override;

    /** PreUpdate で非スケール時間を既存タイマー管理器へ渡す。 */
    void OnTickFrame(const FSubsystemFrameContext& context) noexcept override;

    /** 結び付け済みのタイマー管理器を返し、未初期化なら nullptr を返す。 */
    CTimerManager* GetTimers() noexcept
    {
        return m_Timers;
    }

    /** 結び付け済みのタイマー管理器を返し、未初期化なら nullptr を返す。 */
    const CTimerManager* GetTimers() const noexcept
    {
        return m_Timers;
    }

private:
    /** FApplication が所有するタイマー管理器への非所有参照。 */
    CTimerManager* m_Timers = nullptr;
};

} // namespace acs
