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

} // namespace acs::telemetryfile

