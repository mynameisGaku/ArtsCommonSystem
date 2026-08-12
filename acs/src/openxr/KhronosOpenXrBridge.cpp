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

CKhronosOpenXrBridge::~CKhronosOpenXrBridge() noexcept {
    Shutdown();
}

TResult<void> CKhronosOpenXrBridge::Init(game::EXrPlatform platform) noexcept {
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
    ACS_LOG_INFO("CKhronosOpenXrBridge initialized (extensions=%u)", extension_count);
    return Ok();
}

void CKhronosOpenXrBridge::Shutdown() noexcept {
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

void CKhronosOpenXrBridge::Tick(f32 dt) noexcept {
    (void)dt;
    // renderer 非依存のため graphics binding を持たず、session loop は実行しない。
    // session が無い間は tracking を false とし、pose と controller を zero state へ保つ。
    // instance 初期化後に session が無い状態を一度だけ警告する。
    if (m_bInitialized && !m_bSessionTracking && !m_bTickWarned) {
        ACS_LOG_WARN("CKhronosOpenXrBridge::Tick: OpenXR session loop is not implemented "
                     "(graphics binding + HMD runtime required). HeadPose/Controller "
                     "report the not-tracking zero pose; query IsTracking() and use "
                     "the non-XR fallback path.");
        m_bTickWarned = true;
    }
    m_HeadPose = game::FXrPose{};
    m_Left = game::FXrControllerState{};
    m_Right = game::FXrControllerState{};
}

void CKhronosOpenXrBridge::SetPassthrough(bool b_on) noexcept {
    m_bPassthroughRequested = b_on && m_bPassthroughSupported;
}

} // namespace acs::openxr
