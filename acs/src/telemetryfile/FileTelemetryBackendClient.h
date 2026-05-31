// SPDX-License-Identifier: Apache-2.0
// File-backed telemetry sink for acs::game::IBackendClient.
#pragma once

#include "gameframework/BackendClient.h"

namespace acs::telemetryfile {

class FFileTelemetryBackendClient final : public acs::game::IBackendClient {
public:
    FFileTelemetryBackendClient() noexcept = default;
    ~FFileTelemetryBackendClient() noexcept override;

    FFileTelemetryBackendClient(const FFileTelemetryBackendClient&)            = delete;
    FFileTelemetryBackendClient& operator=(const FFileTelemetryBackendClient&) = delete;
    FFileTelemetryBackendClient(FFileTelemetryBackendClient&&)                 = delete;
    FFileTelemetryBackendClient& operator=(FFileTelemetryBackendClient&&)      = delete;

    acs::TResult<void> Connect(const char* ServerUrl) noexcept override;
    void               Disconnect() noexcept override;
    bool               IsConnected() const noexcept override;
    acs::TResult<void> SendTelemetry(const char* EventName,
                                      const char* JsonPayload) noexcept override;
    void               Tick(acs::f32 Dt) noexcept override;

    const char* Path() const noexcept { return m_Path; }
    acs::u32    WrittenCount() const noexcept { return m_WrittenCount; }

private:
    static constexpr acs::u16 kSubTelemetryFileOpenFailed = 2101;
    static constexpr acs::u16 kSubTelemetryFileWriteFailed = 2102;

    const char* StripFileScheme(const char* ServerUrl) const noexcept;
    void CopyText(char* Dst, acs::usize DstSize, const char* Src) noexcept;
    bool WriteEscaped(const char* Text) noexcept;

    void*    m_File = nullptr;
    char     m_Path[260] = {};
    acs::u32 m_WrittenCount = 0;
};

// =============================================================================
// 既定 IBackendClient 結線ヘルパ (gameframework の provider へ本実装を登録)
// -----------------------------------------------------------------------------
// アプリ起動時に一度だけ `InstallFileTelemetryAsDefault()` を呼ぶと、以降
// `acs::game::GetDefaultBackendClient()` がファイルバック実装 (プロセス共有
// singleton) を返すようになる。これにより上位コードは backend 非依存に
// `GetDefaultBackendClient()` だけで実 backend を取得できる。
// =============================================================================

// プロセス共有の既定 FFileTelemetryBackendClient singleton を返す (provider 実体)。
acs::game::IBackendClient& GetDefaultFileTelemetryBackendClient() noexcept;

// gameframework の backend provider に GetDefaultFileTelemetryBackendClient を登録。
void InstallFileTelemetryAsDefault() noexcept;

} // namespace acs::telemetryfile

