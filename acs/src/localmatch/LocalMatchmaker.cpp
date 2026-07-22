// SPDX-License-Identifier: Apache-2.0
// Deterministic local matchmaker backend for acs::game::IMatchmaker.
#include "localmatch/LocalMatchmaker.h"

#include "foundation/Error.h"

#include <cstring>

namespace acs::localmatch {

FLocalMatchmaker::FLocalMatchmaker(const FLocalMatchmakerConfig& config) noexcept
    : m_MaxRatingDelta(config.MaxRatingDelta) {
}

bool FLocalMatchmaker::StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != 0 && *b != 0) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

void FLocalMatchmaker::CopyText(char* dst, acs::usize dst_size, const char* src) noexcept {
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

FLocalMatchmaker::FEntry* FLocalMatchmaker::FindEntry(
    acs::game::FMatchTicket ticket) noexcept {
    if (!ticket.IsValid()) {
        return nullptr;
    }
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        if (m_Entries[i].bActive && m_Entries[i].Ticket.m_Opaque == ticket.m_Opaque) {
            return &m_Entries[i];
        }
    }
    return nullptr;
}

const FLocalMatchmaker::FEntry* FLocalMatchmaker::FindEntry(
    acs::game::FMatchTicket ticket) const noexcept {
    if (!ticket.IsValid()) {
        return nullptr;
    }
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        if (m_Entries[i].bActive && m_Entries[i].Ticket.m_Opaque == ticket.m_Opaque) {
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
    FEntry* oldest = nullptr;
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        FEntry& entry = m_Entries[i];
        if (entry.RetireSeq == 0) {
            continue; // 生存中 (Searching / Matched 未確定) は再利用しない
        }
        if (oldest == nullptr || entry.RetireSeq < oldest->RetireSeq) {
            oldest = &entry;
        }
    }
    return oldest; // 退役スロットも無ければ nullptr (= 真に満杯)
}

void FLocalMatchmaker::RetireEntry(FEntry& entry) noexcept {
    if (entry.RetireSeq != 0) {
        return; // 既に退役済み: m_ActiveCount を二重減算しない (冪等)
    }
    entry.RetireSeq = m_NextRetire++;
    if (m_ActiveCount > 0) {
        --m_ActiveCount;
    }
}

bool FLocalMatchmaker::IsRatingCompatible(acs::u32 a, acs::u32 b) const noexcept {
    const acs::u32 delta = a > b ? a - b : b - a;
    return delta <= m_MaxRatingDelta;
}

FLocalMatchmaker::FEntry* FLocalMatchmaker::FindCompatibleEntry(const char* mode,
                                                                acs::u32 elo_hint) noexcept {
    for (acs::u32 i = 0; i < kMaxTickets; ++i) {
        FEntry& entry = m_Entries[i];
        if (!entry.bActive || entry.Status != acs::game::EMatchStatus::Searching) {
            continue;
        }
        if (StrEq(entry.Mode, mode) && IsRatingCompatible(entry.EloHint, elo_hint)) {
            return &entry;
        }
    }
    return nullptr;
}

acs::TResult<acs::game::FMatchTicket> FLocalMatchmaker::StartSearch(const char* mode,
                                                                    acs::u32 elo_hint) noexcept {
    if (mode == nullptr || mode[0] == 0) {
        return ACS_ERR(IO, acs::game::FBackendError::kSub_BadArgument,
                       "FLocalMatchmaker::StartSearch requires a mode");
    }

    FEntry* new_entry = FindFreeEntry();
    if (new_entry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchFull,
                       "FLocalMatchmaker ticket pool is full");
    }

    acs::game::FMatchTicket new_ticket{};
    new_ticket.m_Opaque = m_NextTicket++;
    *new_entry = FEntry{};
    new_entry->Ticket = new_ticket;
    new_entry->Status = acs::game::EMatchStatus::Searching;
    new_entry->EloHint = elo_hint;
    new_entry->bActive = true;
    CopyText(new_entry->Mode, sizeof(new_entry->Mode), mode);
    ++m_ActiveCount;

    FEntry* partner = FindCompatibleEntry(mode, elo_hint);
    if (partner != nullptr && partner != new_entry) {
        new_entry->Status = acs::game::EMatchStatus::Matched;
        partner->Status = acs::game::EMatchStatus::Matched;
        new_entry->PartnerTicket = partner->Ticket.m_Opaque;
        partner->PartnerTicket = new_entry->Ticket.m_Opaque;
    }

    return acs::TResult<acs::game::FMatchTicket>(acs::OkInit, new_ticket);
}

acs::TResult<void> FLocalMatchmaker::CancelSearch(acs::game::FMatchTicket ticket) noexcept {
    FEntry* entry = FindEntry(ticket);
    if (entry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchInvalidTicket,
                       "FLocalMatchmaker::CancelSearch invalid ticket");
    }
    // 相手とマッチ済みだった場合、相手をまだ確定していなければ Searching に戻して
    // 再マッチ可能にする (interface 仕様で許容)。相手の PartnerTicket も切る。
    if (entry->PartnerTicket != 0) {
        acs::game::FMatchTicket partner_handle{};
        partner_handle.m_Opaque = entry->PartnerTicket;
        FEntry* partner = FindEntry(partner_handle);
        if (partner != nullptr && partner->RetireSeq == 0
            && partner->Status == acs::game::EMatchStatus::Matched) {
            partner->Status = acs::game::EMatchStatus::Searching;
            partner->PartnerTicket = 0;
            partner->bAccepted = false;
        }
    }
    entry->Status = acs::game::EMatchStatus::Cancelled;
    entry->PartnerTicket = 0;
    // 終端状態なのでスロットを退役させて再利用可能にする (リーク防止)。
    // bActive は true のまま残すので、後続スロットが満杯になるまでは PollStatus が
    // この Cancelled を返し続けられる。
    RetireEntry(*entry);
    return acs::Ok();
}

acs::game::EMatchStatus FLocalMatchmaker::PollStatus(acs::game::FMatchTicket ticket) noexcept {
    const FEntry* entry = FindEntry(ticket);
    return entry != nullptr ? entry->Status : acs::game::EMatchStatus::Failed;
}

acs::TResult<void> FLocalMatchmaker::AcceptMatch(acs::game::FMatchTicket ticket) noexcept {
    FEntry* entry = FindEntry(ticket);
    if (entry == nullptr) {
        return ACS_ERR(IO, kSubLocalMatchInvalidTicket,
                       "FLocalMatchmaker::AcceptMatch invalid ticket");
    }
    if (entry->Status != acs::game::EMatchStatus::Matched) {
        return ACS_ERR(IO, kSubLocalMatchWrongState,
                       "FLocalMatchmaker::AcceptMatch requires Matched status");
    }
    entry->bAccepted = true;
    // 双方が Accept したらマッチ確定 = 終端。両スロットを退役させて再利用可能に
    // する (旧実装は完了パスが一切無く、Matched ticket が永久リークしていた)。
    // Status は Matched のまま残すので PollStatus は確定済みを返し続ける。
    if (entry->PartnerTicket != 0) {
        acs::game::FMatchTicket partner_handle{};
        partner_handle.m_Opaque = entry->PartnerTicket;
        FEntry* partner = FindEntry(partner_handle);
        if (partner != nullptr && partner->bAccepted) {
            RetireEntry(*entry);
            RetireEntry(*partner);
        }
    } else {
        // 相手 ticket が無い (=単独で Matched 化された異常系) は即退役。
        RetireEntry(*entry);
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
