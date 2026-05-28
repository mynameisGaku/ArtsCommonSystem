// SPDX-License-Identifier: Apache-2.0
// HelloLocalMatchmaker - FLocalMatchmakerDemoApp implementation.
#include "LocalMatchmakerDemoApp.h"

#include "localmatch/LocalMatchmaker.h"

#include <cstdio>

namespace hellomatch {

namespace {

const char* StatusName(acs::game::EMatchStatus Status) noexcept {
    switch (Status) {
    case acs::game::EMatchStatus::Searching: return "Searching";
    case acs::game::EMatchStatus::Matched: return "Matched";
    case acs::game::EMatchStatus::Cancelled: return "Cancelled";
    case acs::game::EMatchStatus::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace

int FLocalMatchmakerDemoApp::Run() noexcept {
    std::puts("=== ACS HelloLocalMatchmaker ===");
    std::puts("backend: REAL deterministic local queue (FLocalMatchmaker)");

    acs::localmatch::FLocalMatchmakerConfig Config{};
    Config.MaxRatingDelta = 100;
    acs::localmatch::FLocalMatchmaker Matchmaker(Config);

    auto TicketA = Matchmaker.StartSearch("ranked_1v1", 1500);
    auto TicketB = Matchmaker.StartSearch("ranked_1v1", 1560);
    if (TicketA.IsErr() || TicketB.IsErr()) {
        std::puts("StartSearch: FAILED");
        return 2;
    }

    const acs::game::EMatchStatus StatusA = Matchmaker.PollStatus(TicketA.Value());
    const acs::game::EMatchStatus StatusB = Matchmaker.PollStatus(TicketB.Value());
    auto AcceptA = Matchmaker.AcceptMatch(TicketA.Value());
    auto AcceptB = Matchmaker.AcceptMatch(TicketB.Value());

    auto TicketC = Matchmaker.StartSearch("casual_4v4", 1200);
    if (TicketC.IsErr()) {
        std::puts("StartSearch C: FAILED");
        return 3;
    }
    auto CancelC = Matchmaker.CancelSearch(TicketC.Value());
    const acs::game::EMatchStatus StatusC = Matchmaker.PollStatus(TicketC.Value());

    std::printf("tickets: A=%llu B=%llu C=%llu active=%u\n",
                static_cast<unsigned long long>(TicketA.Value().m_Opaque),
                static_cast<unsigned long long>(TicketB.Value().m_Opaque),
                static_cast<unsigned long long>(TicketC.Value().m_Opaque),
                static_cast<unsigned>(Matchmaker.ActiveTicketCount()));
    std::printf("match  : A=%s B=%s acceptA=%s acceptB=%s\n",
                StatusName(StatusA),
                StatusName(StatusB),
                AcceptA.IsOk() ? "ok" : "fail",
                AcceptB.IsOk() ? "ok" : "fail");
    std::printf("cancel : C=%s cancel=%s\n",
                StatusName(StatusC),
                CancelC.IsOk() ? "ok" : "fail");

    const bool bOk = StatusA == acs::game::EMatchStatus::Matched
                  && StatusB == acs::game::EMatchStatus::Matched
                  && AcceptA.IsOk()
                  && AcceptB.IsOk()
                  && StatusC == acs::game::EMatchStatus::Cancelled
                  && CancelC.IsOk();
    std::puts(bOk ? "result: ALL PASS" : "result: FAILED");
    return bOk ? 0 : 4;
}

} // namespace hellomatch
