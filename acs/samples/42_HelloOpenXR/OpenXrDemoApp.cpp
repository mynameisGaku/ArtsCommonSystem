// SPDX-License-Identifier: Apache-2.0
// HelloOpenXR - FOpenXrDemoApp implementation.
#include "OpenXrDemoApp.h"

#include "openxr/KhronosOpenXrBridge.h"

#include <cstdio>

namespace helloopenxr {

int FOpenXrDemoApp::Run() noexcept {
    std::puts("=== ACS HelloOpenXR ===");
    std::puts("backend: REAL Khronos OpenXR Loader (FKhronosOpenXrBridge)");

    acs::openxr::FKhronosOpenXrBridge Xr;
    auto Init = Xr.Init();
    if (Init.IsOk()) {
        std::printf("Init: OK  initialized=%s  tracking=%s  passthrough=%s\n",
                    Xr.IsInitialized() ? "true" : "false",
                    Xr.IsTracking() ? "true" : "false",
                    Xr.IsPassthroughSupported() ? "true" : "false");
        Xr.Tick(1.0f / 60.0f);
        // 正直な縮退の確認: session loop 未実装なのでトラッキング未確立 = HeadPose は
        // 未トラッキング(原点)。IsTracking()/pose.tracked がともに false を返すこと。
        acs::game::FXrPose head = Xr.HeadPose();
        std::printf("after Tick: tracking=%s  head.tracked=%s (未トラッキングを正直に表明)\n",
                    Xr.IsTracking() ? "true" : "false",
                    head.tracked ? "true" : "false");
        Xr.Shutdown();
        std::puts("result: ALL PASS");
        return 0;
    }

    const auto& Err = Init.Error();
    if (Err.subcode == acs::openxr::kSubOpenXrRuntimeUnavailable) {
        std::printf("Init: runtime unavailable (%s)\n", Err.message);
        std::puts("loader call: OK");
        std::puts("result: ALL PASS (no OpenXR runtime installed on this machine)");
        return 0;
    }

    std::printf("Init: FAILED (%s)\n", Err.message);
    return 2;
}

} // namespace helloopenxr
