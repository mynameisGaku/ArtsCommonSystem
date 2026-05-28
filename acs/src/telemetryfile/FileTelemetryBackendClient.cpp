// SPDX-License-Identifier: Apache-2.0
// File-backed telemetry sink for acs::game::IBackendClient.
#include "telemetryfile/FileTelemetryBackendClient.h"

#include "foundation/Error.h"

#include <cstdio>
#include <cstring>

namespace acs::telemetryfile {

FFileTelemetryBackendClient::~FFileTelemetryBackendClient() noexcept {
    Disconnect();
}

const char* FFileTelemetryBackendClient::StripFileScheme(const char* ServerUrl) const noexcept {
    constexpr const char* kPrefix = "file://";
    constexpr acs::usize kPrefixLen = 7;
    if (ServerUrl == nullptr) {
        return nullptr;
    }
    if (std::strncmp(ServerUrl, kPrefix, kPrefixLen) == 0) {
        return ServerUrl + kPrefixLen;
    }
    return ServerUrl;
}

void FFileTelemetryBackendClient::CopyText(char* Dst,
                                           acs::usize DstSize,
                                           const char* Src) noexcept {
    if (Dst == nullptr || DstSize == 0) {
        return;
    }
    Dst[0] = 0;
    if (Src == nullptr) {
        return;
    }
    std::strncpy(Dst, Src, DstSize - 1);
    Dst[DstSize - 1] = 0;
}

acs::TResult<void> FFileTelemetryBackendClient::Connect(const char* ServerUrl) noexcept {
    const char* Path = StripFileScheme(ServerUrl);
    if (Path == nullptr || Path[0] == 0) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "FFileTelemetryBackendClient::Connect requires a file path");
    }

    Disconnect();
    CopyText(m_Path, sizeof(m_Path), Path);

    FILE* File = nullptr;
    if (fopen_s(&File, m_Path, "ab") != 0 || File == nullptr) {
        m_Path[0] = 0;
        return ACS_ERR(IO, kSubTelemetryFileOpenFailed,
                       "FFileTelemetryBackendClient failed to open telemetry file");
    }

    m_File = File;
    return acs::Ok();
}

void FFileTelemetryBackendClient::Disconnect() noexcept {
    if (m_File != nullptr) {
        FILE* File = static_cast<FILE*>(m_File);
        std::fflush(File);
        std::fclose(File);
        m_File = nullptr;
    }
}

bool FFileTelemetryBackendClient::IsConnected() const noexcept {
    return m_File != nullptr;
}

bool FFileTelemetryBackendClient::WriteEscaped(const char* Text) noexcept {
    FILE* File = static_cast<FILE*>(m_File);
    if (Text == nullptr) {
        return std::fputs("", File) >= 0;
    }
    for (const char* It = Text; *It != 0; ++It) {
        const char C = *It;
        if (C == '"' || C == '\\') {
            if (std::fputc('\\', File) == EOF || std::fputc(C, File) == EOF) {
                return false;
            }
        } else if (C == '\n') {
            if (std::fputs("\\n", File) < 0) {
                return false;
            }
        } else if (C == '\r') {
            if (std::fputs("\\r", File) < 0) {
                return false;
            }
        } else if (std::fputc(C, File) == EOF) {
            return false;
        }
    }
    return true;
}

acs::TResult<void> FFileTelemetryBackendClient::SendTelemetry(const char* EventName,
                                                               const char* JsonPayload) noexcept {
    if (m_File == nullptr) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_NotConnected,
                       "FFileTelemetryBackendClient::SendTelemetry called before Connect");
    }
    if (EventName == nullptr || JsonPayload == nullptr) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "FFileTelemetryBackendClient::SendTelemetry requires event name and payload");
    }

    FILE* File = static_cast<FILE*>(m_File);
    bool bOk = std::fputs("{\"event\":\"", File) >= 0;
    bOk = bOk && WriteEscaped(EventName);
    bOk = bOk && std::fputs("\",\"payload\":", File) >= 0;
    bOk = bOk && std::fputs(JsonPayload, File) >= 0;
    bOk = bOk && std::fputs("}\n", File) >= 0;
    bOk = bOk && std::fflush(File) == 0;
    if (!bOk) {
        return ACS_ERR(IO, kSubTelemetryFileWriteFailed,
                       "FFileTelemetryBackendClient failed to write telemetry event");
    }

    ++m_WrittenCount;
    return acs::Ok();
}

void FFileTelemetryBackendClient::Tick(acs::f32 Dt) noexcept {
    (void)Dt;
}

} // namespace acs::telemetryfile

