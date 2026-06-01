// SPDX-License-Identifier: Apache-2.0
// Real Khronos OpenXR loader backend.
#include "openxr/KhronosOpenXrBridge.h"

#include "foundation/Error.h"
#include "foundation/Log.h"

#include "openxr/openxr.h"

#include <cstring>

namespace acs::openxr {

namespace {

acs::FErrorCode XrError(XrResult Result, acs::u16 Subcode, const char* Message) noexcept {
    (void)Result;
    return ACS_ERR(Generic, Subcode, Message);
}

bool HasExtension(const char* Name) noexcept {
    u32 Count = 0;
    XrResult Result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &Count, nullptr);
    if (Result != XR_SUCCESS || Count == 0) {
        return false;
    }

    XrExtensionProperties Props[128] = {};
    const u32 Cap = Count < 128u ? Count : 128u;
    for (u32 i = 0; i < Cap; ++i) {
        Props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    Result = xrEnumerateInstanceExtensionProperties(nullptr, Cap, &Count, Props);
    if (Result != XR_SUCCESS) {
        return false;
    }
    const u32 n = Count < Cap ? Count : Cap;
    for (u32 i = 0; i < n; ++i) {
        if (std::strcmp(Props[i].extensionName, Name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

FKhronosOpenXrBridge::~FKhronosOpenXrBridge() noexcept {
    Shutdown();
}

TResult<void> FKhronosOpenXrBridge::Init(game::EXrPlatform Platform) noexcept {
    if (m_bInitialized) return Ok();

    u32 ExtensionCount = 0;
    XrResult EnumResult = xrEnumerateInstanceExtensionProperties(nullptr, 0, &ExtensionCount, nullptr);
    if (EnumResult == XR_ERROR_RUNTIME_UNAVAILABLE) {
        return XrError(EnumResult, kSubOpenXrRuntimeUnavailable,
                       "OpenXR loader is linked, but no active OpenXR runtime is installed");
    }
    if (EnumResult != XR_SUCCESS) {
        return XrError(EnumResult, kSubOpenXrInitFailed,
                       "xrEnumerateInstanceExtensionProperties failed");
    }

    XrApplicationInfo AppInfo{};
    std::strncpy(AppInfo.applicationName, "ACS", XR_MAX_APPLICATION_NAME_SIZE - 1);
    AppInfo.applicationVersion = 1;
    std::strncpy(AppInfo.engineName, "ACS", XR_MAX_ENGINE_NAME_SIZE - 1);
    AppInfo.engineVersion = 1;
    AppInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo CreateInfo{};
    CreateInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    CreateInfo.applicationInfo = AppInfo;

    XrInstance Instance = XR_NULL_HANDLE;
    XrResult CreateResult = xrCreateInstance(&CreateInfo, &Instance);
    if (CreateResult == XR_ERROR_RUNTIME_UNAVAILABLE) {
        return XrError(CreateResult, kSubOpenXrRuntimeUnavailable,
                       "OpenXR runtime disappeared while creating instance");
    }
    if (CreateResult != XR_SUCCESS) {
        return XrError(CreateResult, kSubOpenXrInitFailed, "xrCreateInstance failed");
    }

    m_Instance = reinterpret_cast<void*>(Instance);
    m_Platform = Platform;
    m_bInitialized = true;
    m_bPassthroughSupported = HasExtension("XR_FB_passthrough") || HasExtension("XR_HTC_passthrough");
    ACS_LOG_INFO("FKhronosOpenXrBridge initialized (extensions=%u)", ExtensionCount);
    return Ok();
}

void FKhronosOpenXrBridge::Shutdown() noexcept {
    if (m_Instance) {
        xrDestroyInstance(reinterpret_cast<XrInstance>(m_Instance));
        m_Instance = nullptr;
    }
    m_bInitialized = false;
    m_Platform = game::EXrPlatform::Unknown;
    m_HeadPose = game::XrPose{};
    m_Left = game::XrControllerState{};
    m_Right = game::XrControllerState{};
    m_bPassthroughSupported = false;
    m_bPassthroughRequested = false;
    m_bSessionTracking = false;
    m_bTickWarned = false;
}

void FKhronosOpenXrBridge::Tick(f32 Dt) noexcept {
    (void)Dt;
    // 正直化: 実セッションループ (xrCreateSession → xrWaitFrame / xrBeginFrame /
    // xrLocateViews / xrSyncActions / xrLocateSpace) は graphics binding (D3D12/Vulkan)
    // と HMD ランタイムを要求し、本 module は renderer に依存しないため未実装。
    //
    // 旧実装は no-op でありながら IsInitialized()==true を返していたため、消費側は
    // トラッキングが生きていると誤認し、ゼロ初期化された HeadPose / Controller を
    // 「有効なポーズ」として使う偽成功スタブだった。これを除去する:
    //   ・session が無いので m_bSessionTracking は false のまま (IsTrackingActive()
    //     が正直に「未トラッキング」を返す)。
    //   ・ポーズは更新しようがないので、フレームをまたいで古い値が残らないよう
    //     明示的に zero pose (= 未トラッキングを表す原点) へ保つ。
    //   ・session loop 未実装である旨を一度だけ警告し、サイレントに偽ポーズを
    //     供給していた挙動を是正する。
    if (m_bInitialized && !m_bSessionTracking && !m_bTickWarned) {
        ACS_LOG_WARN("FKhronosOpenXrBridge::Tick: OpenXR session loop is not implemented "
                     "(graphics binding + HMD runtime required). HeadPose/Controller "
                     "report the not-tracking zero pose; query IsTrackingActive() and use "
                     "the non-XR fallback path.");
        m_bTickWarned = true;
    }
    m_HeadPose = game::XrPose{};
    m_Left = game::XrControllerState{};
    m_Right = game::XrControllerState{};
}

void FKhronosOpenXrBridge::SetPassthrough(bool bOn) noexcept {
    m_bPassthroughRequested = bOn && m_bPassthroughSupported;
}

} // namespace acs::openxr
