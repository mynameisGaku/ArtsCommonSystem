// SPDX-License-Identifier: Apache-2.0
// Real Khronos OpenXR loader backend for acs::game::IOpenXrBridge.
#pragma once

#include "gameframework/OpenXrBridge.h"

namespace acs::openxr {

inline constexpr acs::u16 kSubOpenXrRuntimeUnavailable = 301;
inline constexpr acs::u16 kSubOpenXrInitFailed         = 302;

class FKhronosOpenXrBridge final : public acs::game::IOpenXrBridge {
public:
    FKhronosOpenXrBridge() noexcept = default;
    ~FKhronosOpenXrBridge() noexcept override;

    acs::TResult<void> Init(acs::game::EXrPlatform platform = acs::game::EXrPlatform::Unknown) noexcept override;
    void Shutdown() noexcept override;
    bool IsInitialized() const noexcept override { return m_bInitialized; }
    acs::game::EXrPlatform ActivePlatform() const noexcept override { return m_Platform; }

    acs::game::XrPose HeadPose() const noexcept override { return m_HeadPose; }
    acs::game::XrControllerState LeftController() const noexcept override { return m_Left; }
    acs::game::XrControllerState RightController() const noexcept override { return m_Right; }
    void Tick(acs::f32 Dt) noexcept override;

    bool IsPassthroughSupported() const noexcept override { return m_bPassthroughSupported; }
    void SetPassthrough(bool bOn) noexcept override;

private:
    void* m_Instance = nullptr; // XrInstance, kept opaque in the public header's TU.
    acs::game::XrPose m_HeadPose{};
    acs::game::XrControllerState m_Left{};
    acs::game::XrControllerState m_Right{};
    acs::game::EXrPlatform m_Platform = acs::game::EXrPlatform::Unknown;
    bool m_bInitialized = false;
    bool m_bPassthroughSupported = false;
    bool m_bPassthroughRequested = false;
};

} // namespace acs::openxr
