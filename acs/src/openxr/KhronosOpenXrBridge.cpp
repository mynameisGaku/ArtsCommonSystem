// SPDX-License-Identifier: Apache-2.0
// Real Khronos OpenXR loader backend.
#include "openxr/KhronosOpenXrBridge.h"

#include "foundation/Error.h"
#include "foundation/Log.h"

#include "openxr/openxr.h"

#include <cstring>

namespace acs::openxr {

namespace {

acs::FErrorCode XrError(XrResult result, acs::u16 subcode, const char* message) noexcept {
    (void)result;
    return ACS_ERR(Generic, subcode, message);
}

bool HasExtension(const char* name) noexcept {
    u32 count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (result != XR_SUCCESS || count == 0) {
        return false;
    }

    XrExtensionProperties props[128] = {};
    const u32 cap = count < 128u ? count : 128u;
    for (u32 i = 0; i < cap; ++i) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    result = xrEnumerateInstanceExtensionProperties(nullptr, cap, &count, props);
    if (result != XR_SUCCESS) {
        return false;
    }
    const u32 n = count < cap ? count : cap;
    for (u32 i = 0; i < n; ++i) {
        if (std::strcmp(props[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

FKhronosOpenXrBridge::~FKhronosOpenXrBridge() noexcept {
    Shutdown();
}

TResult<void> FKhronosOpenXrBridge::Init(game::EXrPlatform platform) noexcept {
    if (m_bInitialized) return Ok();

    u32 extension_count = 0;
    const XrResult enum_result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
    if (enum_result == XR_ERROR_RUNTIME_UNAVAILABLE) {
        return XrError(enum_result, kSubOpenXrRuntimeUnavailable,
                       "OpenXR loader is linked, but no active OpenXR runtime is installed");
    }
    if (enum_result != XR_SUCCESS) {
        return XrError(enum_result, kSubOpenXrInitFailed,
                       "xrEnumerateInstanceExtensionProperties failed");
    }

    XrApplicationInfo app_info{};
    std::strncpy(app_info.applicationName, "ACS", XR_MAX_APPLICATION_NAME_SIZE - 1);
    app_info.applicationVersion = 1;
    std::strncpy(app_info.engineName, "ACS", XR_MAX_ENGINE_NAME_SIZE - 1);
    app_info.engineVersion = 1;
    app_info.apiVersion = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo create_info{};
    create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    create_info.applicationInfo = app_info;

    XrInstance instance = XR_NULL_HANDLE;
    const XrResult create_result = xrCreateInstance(&create_info, &instance);
    if (create_result == XR_ERROR_RUNTIME_UNAVAILABLE) {
        return XrError(create_result, kSubOpenXrRuntimeUnavailable,
                       "OpenXR runtime disappeared while creating instance");
    }
    if (create_result != XR_SUCCESS) {
        return XrError(create_result, kSubOpenXrInitFailed, "xrCreateInstance failed");
    }

    m_Instance = reinterpret_cast<void*>(instance);
    m_Platform = platform;
    m_bInitialized = true;
    m_bPassthroughSupported = HasExtension("XR_FB_passthrough") || HasExtension("XR_HTC_passthrough");
    ACS_LOG_INFO("FKhronosOpenXrBridge initialized (extensions=%u)", extension_count);
    return Ok();
}

void FKhronosOpenXrBridge::Shutdown() noexcept {
    if (m_Instance) {
        xrDestroyInstance(reinterpret_cast<XrInstance>(m_Instance));
        m_Instance = nullptr;
    }
    m_bInitialized = false;
    m_Platform = game::EXrPlatform::Unknown;
    m_HeadPose = game::FXrPose{};
    m_Left = game::FXrControllerState{};
    m_Right = game::FXrControllerState{};
    m_bPassthroughSupported = false;
    m_bPassthroughRequested = false;
    m_bSessionTracking = false;
    m_bTickWarned = false;
}

void FKhronosOpenXrBridge::Tick(f32 dt) noexcept {
    (void)dt;
    // 正直化: 実セッションループ (xrCreateSession → xrWaitFrame / xrBeginFrame /
    // xrLocateViews / xrSyncActions / xrLocateSpace) は graphics binding (D3D12/Vulkan)
    // と HMD ランタイムを要求し、本 module は renderer に依存しないため未実装。
    //
    // 旧実装は no-op でありながら IsInitialized()==true を返していたため、消費側は
    // トラッキングが生きていると誤認し、ゼロ初期化された HeadPose / Controller を
    // 「有効なポーズ」として使う偽成功スタブだった。これを除去する:
    //   ・session が無いので m_bSessionTracking は false のまま (IsTracking()
    //     が共有 IF 越しに正直に「未トラッキング」を返す)。
    //   ・ポーズは更新しようがないので、フレームをまたいで古い値が残らないよう
    //     明示的に zero pose (= 未トラッキングを表す原点) へ保つ。
    //   ・session loop 未実装である旨を一度だけ警告し、サイレントに偽ポーズを
    //     供給していた挙動を是正する。
    if (m_bInitialized && !m_bSessionTracking && !m_bTickWarned) {
        ACS_LOG_WARN("FKhronosOpenXrBridge::Tick: OpenXR session loop is not implemented "
                     "(graphics binding + HMD runtime required). HeadPose/Controller "
                     "report the not-tracking zero pose; query IsTracking() and use "
                     "the non-XR fallback path.");
        m_bTickWarned = true;
    }
    m_HeadPose = game::FXrPose{};
    m_Left = game::FXrControllerState{};
    m_Right = game::FXrControllerState{};
}

void FKhronosOpenXrBridge::SetPassthrough(bool b_on) noexcept {
    m_bPassthroughRequested = b_on && m_bPassthroughSupported;
}

} // namespace acs::openxr
