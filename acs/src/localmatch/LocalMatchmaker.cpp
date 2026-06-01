// SPDX-License-Identifier: Apache-2.0
// Deterministic local matchmaker backend for acs::game::IMatchmaker.
#include "localmatch/LocalMatchmaker.h"

#include "foundation/Error.h"

#include <cstring>

namespace acs::localmatch {

FLocalMatchmaker::FLocalMatchmaker(const FLocalMatchmakerConfig& Config) noexcept
    : m_MaxRatingDelta(Config.MaxRatingDelta) {
}

bool FLocalMatchmaker::StrEq(const char* A, const char* B) noexcept {
    if (A == nullptr || B == nullptr) {
        return false;
    }
    while (*A != 0 && *B != 0) {
        if (*A != *B) {
            return false;
        }
        ++A;
        ++B;
    }
    return *A == 0 && *B == 0;
}

void FLocalMatchmaker::CopyText(char* Dst, acs::usize DstSize, const char* Src) noexcept {
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

FLocalMatchmaker::FEntry* FLocalMatchmaker::FindEntry(
    acs::game::MatchTicket Ticket) noexcept {
    if (!Ticket.IsValid()) {
        return nullptr;
    }
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        if (m_Entries[i].bActive && m_Entries[i].Ticket.m_Opaque == Ticket.m_Opaque) {
            return &m_Entries[i];
        }
    }
    return nullptr;
}

const FLocalMatchmaker::FEntry* FLocalMatchmaker::FindEntry(
    acs::game::MatchTicket Ticket) const noexcept {
    if (!Ticket.IsValid()) {
        return nullptr;
    }
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        if (m_Entries[i].bActive && m_Entries[i].Ticket.m_Opaque == Ticket.m_Opaque) {
            return &m_Entries[i];
        }
    }
    return nullptr;
}

FLocalMatchmaker::FEntry* FLocalMatchmaker::FindFreeEntry() noexcept {
    // まず本当に空いている (一度も使われていない / Clear 済み) スロットを優先。
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        if (!m_Entries[i].bActive) {
            return &m_Entries[i];
        }
    }
    // 空きが無ければ、終端状態で退役済み (RetireSeq != 0) のスロットを再利用する。
    // ここが ticket リーク対策の要: 旧実装は Cancel/完了後もスロットを bActive の
    // まま握りっぱなしで、kMaxTickets 回の StartSearch 後に永久に満杯になっていた。
    // 退役済みのうち最古 (RetireSeq 最小) を選び、新しい終端 ticket の照会期間を
    // できるだけ長く保つ (=PollStatus が直近の Cancelled 等を返せる時間を延ばす)。
    FEntry* Oldest = nullptr;
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        FEntry& Entry = m_Entries[i];
        if (Entry.RetireSeq == 0) {
            continue; // 生存中 (Searching / Matched 未確定) は再利用しない
        }
        if (Oldest == nullptr || Entry.RetireSeq < Oldest->RetireSeq) {
            Oldest = &Entry;
        }
    }
    return Oldest; // 退役スロットも無ければ nullptr (= 真に満杯)
}

void FLocalMatchmaker::RetireEntry(FEntry& Entry) noexcept {
    if (Entry.RetireSeq != 0) {
        return; // 既に退役済み: m_ActiveCount を二重減算しない (冪等)
    }
    Entry.RetireSeq = m_NextRetire++;
    if (m_ActiveCount > 0) {
        --m_ActiveCount;
    }
}

bool FLocalMatchmaker::IsRatingCompatible(acs::u32 A, acs::u32 B) const noexcept {
    const acs::u32 Delta = A > B ? A - B : B - A;
    return Delta <= m_MaxRatingDelta;
}

FLocalMatchmaker::FEntry* FLocalMatchmaker::FindCompatibleEntry(const char* Mode,
                                                                acs::u32 EloHint) noexcept {
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        FEntry& Entry = m_Entries[i];
        if (!Entry.bActive || Entry.Status != acs::game::EMatchStatus::Searching) {
            continue;
        }
        if (StrEq(Entry.Mode, Mode) && IsRatingCompatible(Entry.EloHint, EloHint)) {
            return &Entry;
        }
    }
    return nullptr;
}

acs::TResult<acs::game::MatchTicket> FLocalMatchmaker::StartSearch(const char* Mode,
                                                                    acs::u32 EloHint) noexcept {
    if (Mode == nullptr || Mode[0] == 0) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "FLocalMatchmaker::StartSearch requires a mode");
    }

    FEntry* NewEntry = FindFreeEntry();
    if (NewEntry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchFull,
                       "FLocalMatchmaker ticket pool is full");
    }

    acs::game::MatchTicket NewTicket{};
    NewTicket.m_Opaque = m_NextTicket++;
    *NewEntry = FEntry{};
    NewEntry->Ticket = NewTicket;
    NewEntry->Status = acs::game::EMatchStatus::Searching;
    NewEntry->EloHint = EloHint;
    NewEntry->bActive = true;
    CopyText(NewEntry->Mode, sizeof(NewEntry->Mode), Mode);
    ++m_ActiveCount;

    FEntry* Partner = FindCompatibleEntry(Mode, EloHint);
    if (Partner != nullptr && Partner != NewEntry) {
        NewEntry->Status = acs::game::EMatchStatus::Matched;
        Partner->Status = acs::game::EMatchStatus::Matched;
        NewEntry->PartnerTicket = Partner->Ticket.m_Opaque;
        Partner->PartnerTicket = NewEntry->Ticket.m_Opaque;
    }

    return acs::TResult<acs::game::MatchTicket>(acs::OkInit, NewTicket);
}

acs::TResult<void> FLocalMatchmaker::CancelSearch(acs::game::MatchTicket Ticket) noexcept {
    FEntry* Entry = FindEntry(Ticket);
    if (Entry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchInvalidTicket,
                       "FLocalMatchmaker::CancelSearch invalid ticket");
    }
    // 相手とマッチ済みだった場合、相手をまだ確定していなければ Searching に戻して
    // 再マッチ可能にする (interface 仕様で許容)。相手の PartnerTicket も切る。
    if (Entry->PartnerTicket != 0) {
        acs::game::MatchTicket PartnerHandle{};
        PartnerHandle.m_Opaque = Entry->PartnerTicket;
        FEntry* Partner = FindEntry(PartnerHandle);
        if (Partner != nullptr && Partner->RetireSeq == 0
            && Partner->Status == acs::game::EMatchStatus::Matched) {
            Partner->Status = acs::game::EMatchStatus::Searching;
            Partner->PartnerTicket = 0;
            Partner->bAccepted = false;
        }
    }
    Entry->Status = acs::game::EMatchStatus::Cancelled;
    Entry->PartnerTicket = 0;
    // 終端状態なのでスロットを退役させて再利用可能にする (リーク防止)。
    // bActive は true のまま残すので、後続スロットが満杯になるまでは PollStatus が
    // この Cancelled を返し続けられる。
    RetireEntry(*Entry);
    return acs::Ok();
}

acs::game::EMatchStatus FLocalMatchmaker::PollStatus(acs::game::MatchTicket Ticket) noexcept {
    const FEntry* Entry = FindEntry(Ticket);
    return Entry != nullptr ? Entry->Status : acs::game::EMatchStatus::Failed;
}

acs::TResult<void> FLocalMatchmaker::AcceptMatch(acs::game::MatchTicket Ticket) noexcept {
    FEntry* Entry = FindEntry(Ticket);
    if (Entry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchInvalidTicket,
                       "FLocalMatchmaker::AcceptMatch invalid ticket");
    }
    if (Entry->Status != acs::game::EMatchStatus::Matched) {
        return ACS_ERR(IO, kSubLocalMatchWrongState,
                       "FLocalMatchmaker::AcceptMatch requires Matched status");
    }
    Entry->bAccepted = true;
    // 双方が Accept したらマッチ確定 = 終端。両スロットを退役させて再利用可能に
    // する (旧実装は完了パスが一切無く、Matched ticket が永久リークしていた)。
    // Status は Matched のまま残すので PollStatus は確定済みを返し続ける。
    if (Entry->PartnerTicket != 0) {
        acs::game::MatchTicket PartnerHandle{};
        PartnerHandle.m_Opaque = Entry->PartnerTicket;
        FEntry* Partner = FindEntry(PartnerHandle);
        if (Partner != nullptr && Partner->bAccepted) {
            RetireEntry(*Entry);
            RetireEntry(*Partner);
        }
    } else {
        // 相手 ticket が無い (=単独で Matched 化された異常系) は即退役。
        RetireEntry(*Entry);
    }
    return acs::Ok();
}

void FLocalMatchmaker::Clear() noexcept {
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        m_Entries[i] = FEntry{};
    }
    m_NextTicket = 1;
    m_NextRetire = 1;
    m_ActiveCount = 0;
}

} // namespace acs::localmatch
