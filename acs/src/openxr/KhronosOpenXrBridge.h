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

    // 実トラッキングが生きているか (= xrCreateSession + frame/view/action loop が
    // 確立済みか) の正直な問い合わせ。本 backend は loader/instance seam のみを
    // 所有し session loop は未実装なので、現状は常に false を返す。HeadPose() /
    // LeftController() / RightController() が返すゼロポーズを「有効なトラッキング
    // 値」と誤認しないために、消費側はこのフラグを見て fallback を選ぶこと。
    // (IsInitialized() は instance 生成成功を表すだけで、トラッキング有効性とは別。)
    bool IsTrackingActive() const noexcept { return m_bSessionTracking; }

private:
    void* m_Instance = nullptr; // XrInstance, kept opaque in the public header's TU.
    acs::game::XrPose m_HeadPose{};
    acs::game::XrControllerState m_Left{};
    acs::game::XrControllerState m_Right{};
    acs::game::EXrPlatform m_Platform = acs::game::EXrPlatform::Unknown;
    bool m_bInitialized = false;
    bool m_bPassthroughSupported = false;
    bool m_bPassthroughRequested = false;
    // 実セッション (xrCreateSession + frame/view/action loop) が確立してポーズが
    // 実際に更新されているか。session loop は本 TU では未実装なので常に false。
    bool m_bSessionTracking = false;
    // Tick() の「session loop 未実装」警告を一度だけ出すためのラッチ。
    bool m_bTickWarned = false;
};

// =============================================================================
// 既定 OpenXrBridge への結線 (gameframework provider への登録 helper)
// -----------------------------------------------------------------------------
// gameframework は本モジュール (ACS::OpenXr) に依存できない (循環依存) ため、結線は
// backend 側 (本モジュール) から `acs::game::SetOpenXrBridgeProvider()` を呼んで行う。
// アプリは起動時に一度 `acs::openxr::InstallOpenXrAsDefault()` を呼ぶだけで、以降は
// backend 非依存に `acs::game::GetDefaultOpenXrBridge()` で実 Khronos OpenXR bridge を
// 取得できる。
// =============================================================================

// プロセス共有 singleton の実 Khronos bridge を返す。初回アクセス時に Init() を 1 回
// 走らせる (失敗しても参照は返す)。
acs::game::IOpenXrBridge& GetDefaultKhronosOpenXrBridge() noexcept;

// gameframework の既定 provider に GetDefaultKhronosOpenXrBridge を登録する。
void InstallOpenXrAsDefault() noexcept;

} // namespace acs::openxr
