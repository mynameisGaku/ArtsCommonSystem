// SPDX-License-Identifier: Apache-2.0
// WindowsCrashReporterDefault — CWindowsCrashReporter を gameframework の既定
// CrashReporter provider へ結線する。
//
// gameframework は ACS::CrashWin に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetCrashReporterProvider()` を呼んで行う。アプリは
// 起動時に一度 `acs::crashwin::InstallWindowsCrashReporterAsDefault()` を呼ぶだけ
// で、以降は backend 非依存に `acs::game::GetDefaultCrashReporter()` で実 Windows
// DbgHelp backend を取得できる。
//
// provider が返す backend はプロセス共有 singleton。初回アクセス時に Init() を 1
// 回走らせ、すぐ使える状態で返す。CWindowsCrashReporter::Init() は product id /
// version の non-empty を要求するため、ここでは ACS 既定のプレースホルダを渡す。
// タイトル側はこの後 backend の API を直接叩く際に SetUserId 等で文脈を補える。
#include "crashwin/WindowsCrashReporter.h"

namespace acs::crashwin {

/** プロセス共有の既定 CWindowsCrashReporter singleton を返す (provider の実体)。 */
acs::game::ICrashReporterBackend& GetDefaultWindowsCrashReporter() noexcept {
    // Meyers singleton。プロセス内 1 個の Windows backend を既定として共有する。
    static CWindowsCrashReporter s_backend;
    // 実際の初期化状態を判定に使い、明示 Shutdown 後や失敗後も再取得で復帰させる。
    // Init() は product id / version の non-empty を要求するので既定値を渡す。
    if (!s_backend.IsAvailable()) {
        // 失敗しても backend 参照は返す (呼び出し側が IsAvailable 等で判断)。
        (void)s_backend.Init("com.acs.app", "0.0.0");
    }
    return s_backend;
}

/** gameframework の CrashReporter provider に本 backend を登録する。 */
void InstallWindowsCrashReporterAsDefault() noexcept {
    acs::game::SetCrashReporterProvider(&GetDefaultWindowsCrashReporter);
}

} // namespace acs::crashwin
