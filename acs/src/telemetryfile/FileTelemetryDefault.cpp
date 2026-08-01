// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// FileTelemetryDefault — CFileTelemetryBackendClient を gameframework の既定
// IBackendClient provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::TelemetryFile に依存できない (循環依存) ため、結線は
// backend 側 (本 TU) から `acs::game::SetBackendClientProvider()` を呼んで行う。
// アプリは起動時に一度 `acs::telemetryfile::InstallFileTelemetryAsDefault()` を
// 呼ぶだけで、以降は backend 非依存に `acs::game::GetDefaultBackendClient()` で実
// ファイルテレメトリ backend を取得できる。
//
// provider が返す client はプロセス共有 singleton。ファイルバック実装は
// 既定構築だけで即利用可能 (出力先は呼び出し側が Connect("file://...") で指定する)
// ので、Lua VM のような初回 Init は不要 (function-local static の構築自体は
// thread-safe)。
// =============================================================================
#include "telemetryfile/FileTelemetryBackendClient.h"
#include "gameframework/BackendClient.h"

namespace acs::telemetryfile {

acs::game::IBackendClient& GetDefaultFileTelemetryBackendClient() noexcept {
    // Meyers singleton。プロセス内 1 個のファイルテレメトリ client を既定として
    // 共有する。Connect は呼び出し側が出力先を決めた時点で行う。
    static CFileTelemetryBackendClient s_client;
    return s_client;
}

void InstallFileTelemetryAsDefault() noexcept {
    acs::game::SetBackendClientProvider(&GetDefaultFileTelemetryBackendClient);
}

} // namespace acs::telemetryfile
