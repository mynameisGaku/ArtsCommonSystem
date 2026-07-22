// SPDX-License-Identifier: Apache-2.0
// HelloTelemetryFile - FTelemetryFileDemoApp implementation.
#include "TelemetryFileDemoApp.h"

#include "gameframework/TelemetryDirector.h"
#include "telemetryfile/FileTelemetryBackendClient.h"

#include <cstdio>
#include <cstring>

namespace hellotelemetry {

namespace {

bool FileContains(const char* Path, const char* Needle) noexcept {
    FILE* File = nullptr;
    if (fopen_s(&File, Path, "rb") != 0 || File == nullptr) {
        return false;
    }

    char Buffer[4096] = {};
    const size_t Count = std::fread(Buffer, 1, sizeof(Buffer) - 1, File);
    std::fclose(File);
    Buffer[Count] = 0;
    return std::strstr(Buffer, Needle) != nullptr;
}

} // namespace

int FTelemetryFileDemoApp::Run() noexcept {
    constexpr const char* kPath = "hello_telemetry.jsonl";
    std::remove(kPath);

    std::puts("=== ACS HelloTelemetryFile ===");
    std::puts("backend: REAL file JSONL telemetry sink (FFileTelemetryBackendClient)");

    acs::telemetryfile::FFileTelemetryBackendClient Backend;
    auto Connect = Backend.Connect(kPath);
    if (Connect.IsErr()) {
        std::printf("Connect: FAILED (%s)\n", Connect.Error().message);
        return 2;
    }

    acs::game::FTelemetryDirector Telemetry;
    Telemetry.Init(&Backend);
    Telemetry.TrackEvent("level_started", "{\"level\":1}", acs::game::EEventPriority::Info, "gameplay");
    Telemetry.TrackEvent("player_died", "{\"reason\":\"spike\"}", acs::game::EEventPriority::Important, "gameplay");
    Telemetry.Flush();

    const acs::u32 SentCount = Telemetry.SentCount();
    const acs::u32 PendingCount = Telemetry.PendingCount();
    const acs::u32 FailedCount = Telemetry.FailedCount();
    const bool bCountsOk = SentCount == 2
                         && PendingCount == 0
                         && FailedCount == 0
                         && Backend.WrittenCount() == 2;
    Telemetry.Shutdown();
    Backend.Disconnect();

    const bool bLevelFound = FileContains(kPath, "\"event\":\"level_started\"");
    const bool bDeathFound = FileContains(kPath, "\"event\":\"player_died\"");

    std::printf("file  : %s\n", kPath);
    std::printf("counts: sent=%u pending=%u failed=%u written=%u\n",
                static_cast<unsigned>(SentCount),
                static_cast<unsigned>(PendingCount),
                static_cast<unsigned>(FailedCount),
                static_cast<unsigned>(Backend.WrittenCount()));
    std::printf("events: level_started=%s player_died=%s\n",
                bLevelFound ? "ok" : "missing",
                bDeathFound ? "ok" : "missing");

    const bool bOk = bCountsOk && bLevelFound && bDeathFound;
    std::puts(bOk ? "result: ALL PASS" : "result: FAILED");
    return bOk ? 0 : 3;
}

} // namespace hellotelemetry
