// SPDX-License-Identifier: Apache-2.0
// Deterministic local matchmaker backend for acs::game::IMatchmaker.
#pragma once

#include "gameframework/BackendClient.h"

namespace acs::localmatch {

struct FLocalMatchmakerConfig {
    acs::u32 MaxRatingDelta = 150;
};

class FLocalMatchmaker final : public acs::game::IMatchmaker {
public:
    FLocalMatchmaker() noexcept = default;
    explicit FLocalMatchmaker(const FLocalMatchmakerConfig& Config) noexcept;
    ~FLocalMatchmaker() noexcept override = default;

    FLocalMatchmaker(const FLocalMatchmaker&)            = delete;
    FLocalMatchmaker& operator=(const FLocalMatchmaker&) = delete;
    FLocalMatchmaker(FLocalMatchmaker&&)                 = delete;
    FLocalMatchmaker& operator=(FLocalMatchmaker&&)      = delete;

    acs::TResult<acs::game::MatchTicket> StartSearch(const char* Mode,
                                                      acs::u32 EloHint) noexcept override;
    acs::TResult<void>                   CancelSearch(acs::game::MatchTicket Ticket) noexcept override;
    acs::game::EMatchStatus              PollStatus(acs::game::MatchTicket Ticket) noexcept override;
    acs::TResult<void>                   AcceptMatch(acs::game::MatchTicket Ticket) noexcept override;

    void Clear() noexcept;
    acs::u32 ActiveTicketCount() const noexcept { return m_ActiveCount; }

private:
    struct FEntry {
        acs::game::MatchTicket Ticket{};
        acs::game::EMatchStatus Status = acs::game::EMatchStatus::Failed;
        char Mode[64] = {};
        acs::u32 EloHint = 0;
        acs::u64 PartnerTicket = 0;
        // 退役 (Retire) シーケンス番号。終端状態 (Cancelled / 双方 Accept 済み) に
        // 入った瞬間に非ゼロ値を割り当てる。bActive=true のまま退役することで
        // PollStatus は終端 Status を返し続けつつ、スロットが満杯のときに最古の
        // 退役スロットを FindFreeEntry が再利用できる (= ticket リーク防止)。
        // 値が小さいほど古い (m_NextRetire の昇順)。0 は「未退役 (生存中)」。
        acs::u64 RetireSeq = 0;
        bool bActive = false;
        bool bAccepted = false;
    };

    static constexpr acs::u32 kMaxTickets = 32;
    static constexpr acs::u16 kSubLocalMatchFull = 2201;
    static constexpr acs::u16 kSubLocalMatchInvalidTicket = 2202;
    static constexpr acs::u16 kSubLocalMatchWrongState = 2203;

    static bool StrEq(const char* A, const char* B) noexcept;
    static void CopyText(char* Dst, acs::usize DstSize, const char* Src) noexcept;

    FEntry* FindEntry(acs::game::MatchTicket Ticket) noexcept;
    const FEntry* FindEntry(acs::game::MatchTicket Ticket) const noexcept;
    FEntry* FindFreeEntry() noexcept;
    FEntry* FindCompatibleEntry(const char* Mode, acs::u32 EloHint) noexcept;
    bool IsRatingCompatible(acs::u32 A, acs::u32 B) const noexcept;

    // 終端状態に入った Entry を退役させてスロットを再利用可能にする
    // (m_ActiveCount を 1 回だけ減算)。既に退役済みなら何もしない (冪等)。
    void RetireEntry(FEntry& Entry) noexcept;

    FEntry m_Entries[kMaxTickets] = {};
    acs::u64 m_NextTicket = 1;
    acs::u64 m_NextRetire = 1;
    acs::u32 m_ActiveCount = 0;
    acs::u32 m_MaxRatingDelta = 150;
};

// =============================================================================
// 既定 IMatchmaker 結線ヘルパ (gameframework の provider へ本実装を登録)
// -----------------------------------------------------------------------------
// アプリ起動時に一度だけ `InstallLocalMatchmakerAsDefault()` を呼ぶと、以降
// `acs::game::GetDefaultMatchmaker()` がローカル matchmaker 実装 (プロセス共有
// singleton) を返すようになる。これにより上位コードは backend 非依存に
// `GetDefaultMatchmaker()` だけで実 matchmaker を取得できる。
// =============================================================================

// プロセス共有の既定 FLocalMatchmaker singleton を返す (provider 実体)。
acs::game::IMatchmaker& GetDefaultLocalMatchmaker() noexcept;

// gameframework の matchmaker provider に GetDefaultLocalMatchmaker を登録する。
void InstallLocalMatchmakerAsDefault() noexcept;

} // namespace acs::localmatch
