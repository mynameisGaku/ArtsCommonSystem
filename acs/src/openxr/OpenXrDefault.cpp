// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// OpenXrDefault — FKhronosOpenXrBridge を gameframework の既定 provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::OpenXr に依存できない (循環依存) ため、結線は backend 側
// (本 TU) から `acs::game::SetOpenXrBridgeProvider()` を呼んで行う。アプリは起動時
// に一度 `acs::openxr::InstallOpenXrAsDefault()` を呼ぶだけで、以降は backend 非依存
// に `acs::game::GetDefaultOpenXrBridge()` で実 Khronos OpenXR bridge を取得できる。
//
// provider が返す bridge はプロセス共有 singleton。初回アクセス時に Init() を 1 回
// 走らせ、すぐ使える状態で返す (HMD / ランタイム未接続でも参照は返り、上位は
// IsInitialized() で fallback を判断する)。
// =============================================================================
#include "openxr/KhronosOpenXrBridge.h"
#include "gameframework/OpenXrBridge.h"

namespace acs::openxr {

acs::game::IOpenXrBridge& GetDefaultKhronosOpenXrBridge() noexcept {
    // Meyers singleton。プロセス内 1 個の実 Khronos bridge を既定として共有する。
    static FKhronosOpenXrBridge s_bridge;
    // 初回のみ Init。ACS のこの層は単一スレッド初期化を前提とするため、素朴な
    // フラグで十分 (function-local static の構築自体は thread-safe)。
    static bool s_inited = false;
    if (!s_inited) {
        // platform は自動検出に任せる (Unknown)。失敗しても bridge 参照は返す
        // (呼び出し側が IsInitialized() で判断し fallback する)。
        (void)s_bridge.Init();
        s_inited = true;
    }
    return s_bridge;
}

void InstallOpenXrAsDefault() noexcept {
    acs::game::SetOpenXrBridgeProvider(&GetDefaultKhronosOpenXrBridge);
}

} // namespace acs::openxr
