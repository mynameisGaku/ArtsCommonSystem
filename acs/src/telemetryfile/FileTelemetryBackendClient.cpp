// SPDX-License-Identifier: Apache-2.0
// File-backed telemetry sink for acs::game::IBackendClient.
#include "telemetryfile/FileTelemetryBackendClient.h"

#include "foundation/Error.h"

#include <cstdio>
#include <cstring>

namespace acs::telemetryfile {

CFileTelemetryBackendClient::~CFileTelemetryBackendClient() noexcept {
    Disconnect();
}

const char* CFileTelemetryBackendClient::StripFileScheme(const char* server_url) const noexcept {
    constexpr const char* kPrefix = "file://";
    constexpr acs::usize kPrefixLen = 7;
    if (server_url == nullptr) {
        return nullptr;
    }
    if (std::strncmp(server_url, kPrefix, kPrefixLen) == 0) {
        return server_url + kPrefixLen;
    }
    return server_url;
}

void CFileTelemetryBackendClient::CopyText(char* dst,
                                           acs::usize dst_size,
                                           const char* src) noexcept {
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    dst[0] = 0;
    if (src == nullptr) {
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

acs::TResult<void> CFileTelemetryBackendClient::Connect(const char* server_url) noexcept {
    const char* const path = StripFileScheme(server_url);
    if (path == nullptr || path[0] == 0) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "CFileTelemetryBackendClient::Connect requires a file path");
    }

    Disconnect();
    CopyText(m_Path, sizeof(m_Path), path);

    FILE* file = nullptr;
    if (fopen_s(&file, m_Path, "ab") != 0 || file == nullptr) {
        m_Path[0] = 0;
        return ACS_ERR(IO, kSubTelemetryFileOpenFailed,
                       "CFileTelemetryBackendClient failed to open telemetry file");
    }

    m_File = file;
    return acs::Ok();
}

void CFileTelemetryBackendClient::Disconnect() noexcept {
    if (m_File != nullptr) {
        FILE* const file = static_cast<FILE*>(m_File);
        std::fflush(file);
        std::fclose(file);
        m_File = nullptr;
    }
}

bool CFileTelemetryBackendClient::IsConnected() const noexcept {
    return m_File != nullptr;
}

bool CFileTelemetryBackendClient::WriteEscaped(const char* text) noexcept {
    FILE* const file = static_cast<FILE*>(m_File);
    if (text == nullptr) {
        return std::fputs("", file) >= 0;
    }
    for (const char* it = text; *it != 0; ++it) {
        const char c = *it;
        if (c == '"' || c == '\\') {
            if (std::fputc('\\', file) == EOF || std::fputc(c, file) == EOF) {
                return false;
            }
        } else if (c == '\n') {
            if (std::fputs("\\n", file) < 0) {
                return false;
            }
        } else if (c == '\r') {
            if (std::fputs("\\r", file) < 0) {
                return false;
            }
        } else if (std::fputc(c, file) == EOF) {
            return false;
        }
    }
    return true;
}

acs::TResult<void> CFileTelemetryBackendClient::SendTelemetry(const char* event_name,
                                                               const char* json_payload) noexcept {
    if (m_File == nullptr) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_NotConnected,
                       "CFileTelemetryBackendClient::SendTelemetry called before Connect");
    }
    if (event_name == nullptr || json_payload == nullptr) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "CFileTelemetryBackendClient::SendTelemetry requires event name and payload");
    }

    FILE* const file = static_cast<FILE*>(m_File);
    bool b_ok = std::fputs("{\"event\":\"", file) >= 0;
    b_ok = b_ok && WriteEscaped(event_name);
    b_ok = b_ok && std::fputs("\",\"payload\":", file) >= 0;
    b_ok = b_ok && std::fputs(json_payload, file) >= 0;
    b_ok = b_ok && std::fputs("}\n", file) >= 0;
    b_ok = b_ok && std::fflush(file) == 0;
    if (!b_ok) {
        return ACS_ERR(IO, kSubTelemetryFileWriteFailed,
                       "CFileTelemetryBackendClient failed to write telemetry event");
    }

    ++m_WrittenCount;
    return acs::Ok();
}

void CFileTelemetryBackendClient::Tick(acs::f32 dt) noexcept {
    (void)dt;
}

} // namespace acs::telemetryfile
