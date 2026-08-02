// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — CPartySystem 実装
//
// state machine + メンバ/フレンドリスト管理は完全実装。実 SDK 呼び出し
// (FSteamworksBridge / EOS / PSN / Xbox / NSO) は seam として未接続で、Joining /
// Leaving は Tick で仮想完了する。これにより呼び出し側 (CGame / Scene) は本 system を
// 通常通り使い始めることができ、bridge を差し替えるだけで実プラットフォーム動作する設計。
//
// 設計メモ:
//   ・SDK 完了 (kPendingDurationSec 経過) は **常に成功扱い** にしている。実 SDK
//     接続後は Tick 中に bridge.PollResult() を見て成功/失敗を分岐する。失敗時は
//     state を Solo に戻すリセット動作を入れる (現状は未実装)。
//   ・タイムアウト閾値は短め (1.5 秒) に設定。これは UI のスピナー演出を兼ねた
//     疑似遅延であり、実 SDK 接続後は外部の真の completion event に置き換わる。
//   ・メンバ / フレンド比較は Pillar O FEntitlement と同じく自前 StrEq で実施。
//     STL 禁止 + <cstring> も避ける方針。
#include "gameframework/PartySystem.h"

namespace acs::game {

namespace {

/** 仮想 SDK 完了までの待ち時間 (秒)。暫定値で、real bridge 差し替え後は使用しない。 */
constexpr f32 kPendingDurationSec = 1.5f;

/**
 * const char* の安全比較を行う。
 *
 * @details どちらかが nullptr なら false。終端ヌルまで一致比較する (STL 不使用)。
 * @param a 比較対象その 1。
 * @param b 比較対象その 2。
 * @return 内容が一致すれば true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace

void CPartySystem::EmplaceSelfAsLeader() noexcept {
    // 実装時は FSteamworksBridge::GetLocalPlayerId() / GetLocalDisplayName()
    // から取得して入れ替える。現状は固定文字列リテラル (寿命無限) を使用。
    FPartyMember self{};
    self.player_id    = "local:self";
    self.display_name = "You";
    self.is_leader    = true;
    self.is_ready     = false;
    m_Members.PushBack(self);
}

TResult<void> CPartySystem::CreateParty(const char* party_name) noexcept {
    if (_state != EPartyState::Solo) {
        // 既にパーティ所属中 / 状態遷移中での再要求は許可しない。先に LeaveParty を
        // 完了させること。Generic + subcode 1 = "invalid state for CreateParty"。
        return ACS_ERR(Generic, 1, "CPartySystem::CreateParty: not in Solo state");
    }
    if (party_name == nullptr) {
        // 空名は将来 SDK によっては許可するが、bridge 統一のため最低限名前必須。
        return ACS_ERR(Generic, 2, "CPartySystem::CreateParty: party_name is null");
    }
    m_PartyName    = party_name;
    m_PartyId      = nullptr;  // 自分作成時は SDK が後で割り当てる想定
    m_PendingTimer = 0.0f;
    _state         = EPartyState::Joining;
    // real bridge 接続後は FSteamworksBridge.CreateLobby(party_name) を発行し、
    // Tick() 中に PollResult を見て InParty 遷移する。失敗時は Solo に戻す。
    return Ok();
}

TResult<void> CPartySystem::JoinParty(const char* party_id) noexcept {
    if (_state != EPartyState::Solo) {
        return ACS_ERR(Generic, 3, "CPartySystem::JoinParty: not in Solo state");
    }
    if (party_id == nullptr) {
        return ACS_ERR(Generic, 4, "CPartySystem::JoinParty: party_id is null");
    }
    m_PartyName    = nullptr;
    m_PartyId      = party_id;
    m_PendingTimer = 0.0f;
    _state         = EPartyState::Joining;
    // real bridge 接続後は FSteamworksBridge.JoinLobby(party_id) を発行する。
    return Ok();
}

TResult<void> CPartySystem::LeaveParty() noexcept {
    if (_state != EPartyState::InParty) {
        // Joining / Leaving / Solo からの離脱は no-op ではなくエラーで返す
        // (上位レイヤで状態整合を取れていない時に気付けるように)。
        return ACS_ERR(Generic, 5, "CPartySystem::LeaveParty: not in InParty state");
    }
    m_PendingTimer = 0.0f;
    _state         = EPartyState::Leaving;
    // real bridge 接続後は FSteamworksBridge.LeaveLobby(m_PartyId) を発行する。
    return Ok();
}

TResult<void> CPartySystem::InviteFriend(const char* friend_id) noexcept {
    if (_state != EPartyState::InParty) {
        return ACS_ERR(Generic, 6, "CPartySystem::InviteFriend: not in InParty state");
    }
    if (friend_id == nullptr) {
        return ACS_ERR(Generic, 7, "CPartySystem::InviteFriend: friend_id is null");
    }
    // フレンドリスト内に存在するかは「警告のみ」で弾かない。プラットフォーム
    // 上は friend でない相手にも (公開設定次第で) invite を送れるケースがあり、
    // 厳格にローカル list と突き合わせると有効な招待を弾く可能性があるため。
    // 一方で moderation / blocking 判定はここでは行わない (将来の別モジュール)。
    (void)friend_id;
    // real bridge 接続後は FSteamworksBridge.InviteFriendToLobby(m_PartyId, friend_id)
    // を発行する。結果通知 (送信失敗 / 受信側拒否 / 受信側 accept) は bridge イベントで
    // 非同期に流れてくる想定で、本 API は「送信できたか」だけを返す。
    return Ok();
}

u32 CPartySystem::MemberCount() const noexcept {
    return static_cast<u32>(m_Members.Size());
}

const FPartyMember* CPartySystem::Members() const noexcept {
    return m_Members.Data();
}

u32 CPartySystem::FindMember(const char* player_id) const noexcept {
    if (player_id == nullptr) return kInvalidIndex;
    const usize n = m_Members.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Members[i].player_id, player_id)) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIndex;
}

bool CPartySystem::HasMember(const char* player_id) const noexcept {
    return FindMember(player_id) != kInvalidIndex;
}

const FPartyMember* CPartySystem::GetMember(u32 index) const noexcept {
    // kInvalidIndex を含む範囲外は安全に nullptr (FindMember の戻り値を
    // チェック無しで渡しても落ちないように)。
    if (static_cast<usize>(index) >= m_Members.Size()) return nullptr;
    return &m_Members[index];
}

TResult<void> CPartySystem::AddMember(const FPartyMember& member) noexcept {
    if (member.player_id == nullptr) {
        // SDK 取得失敗時の nullptr 流入で roster を壊さない。Generic+8。
        return ACS_ERR(Generic, 8, "CPartySystem::AddMember: player_id is null");
    }
    // 同 player_id の二重追加は上書きせずエラーで弾く (呼び出し側が
    // accept イベントを取りこぼして再送した等を検知できるように)。Generic+9。
    if (FindMember(member.player_id) != kInvalidIndex) {
        return ACS_ERR(Generic, 9, "CPartySystem::AddMember: duplicate player_id");
    }
    m_Members.PushBack(member);
    return Ok();
}

bool CPartySystem::RemoveMember(const char* player_id) noexcept {
    const u32 idx = FindMember(player_id);
    if (idx == kInvalidIndex) {
        // nullptr / 不在は no-op。呼び出し側でリトライ判定しやすいよう false。
        return false;
    }
    // 順序を保ったまま前詰め (RemoveAtSwap だとリーダー先頭の並びが崩れ、
    // UI のメンバ表示順が安定しないため線形シフトを選択)。
    const usize n = m_Members.Size();
    for (usize i = static_cast<usize>(idx) + 1; i < n; ++i) {
        m_Members[i - 1] = m_Members[i];
    }
    m_Members.PopBack();
    return true;
}

void CPartySystem::Tick(f32 dt) noexcept {
    // Solo / InParty では時間進行は不要 (将来 idle 検出を入れる場合はここを拡張)。
    if (_state == EPartyState::Joining) {
        m_PendingTimer += dt;
        if (m_PendingTimer >= kPendingDurationSec) {
            // 仮想 SDK 完了: メンバリストに自分をリーダーとして登録、InParty へ。
            // 既存メンバが残っている可能性は通常無いが、防御的に Clear してから登録。
            m_Members.Clear();
            EmplaceSelfAsLeader();
            m_PendingTimer = 0.0f;
            _state         = EPartyState::InParty;
        }
        return;
    }
    if (_state == EPartyState::Leaving) {
        m_PendingTimer += dt;
        if (m_PendingTimer >= kPendingDurationSec) {
            // 仮想 SDK 完了: メンバリストをクリアして Solo に戻す。
            m_Members.Clear();
            m_PartyName    = nullptr;
            m_PartyId      = nullptr;
            m_PendingTimer = 0.0f;
            _state         = EPartyState::Solo;
        }
        return;
    }
}

void CPartySystem::AddFriend(const FFriend& f) noexcept {
    if (f.platform_id == nullptr) {
        // SDK 取得失敗時の nullptr 流入で list を壊さない (Pillar O と同じ防御)。
        return;
    }
    // 重複 platform_id は上書きせず単純追加 (Pillar S 側で dedup する想定)。
    // 必要なら同 ID 検出ループを将来追加するが、線形走査コストを増やすので保留。
    m_Friends.PushBack(f);
}

u32 CPartySystem::FriendCount() const noexcept {
    // Pillar O FEntitlement と同じく u32 範囲で十分 (現実的に friend list は
    // SDK 制限内: 大規模 SNS の Steam で 2000、PSN で 2000、Xbox で 1000 等)。
    return static_cast<u32>(m_Friends.Size());
}

const FFriend* CPartySystem::Friends() const noexcept {
    return m_Friends.Data();
}

/**
 * フレンドリスト内に指定 platform_id が含まれるかを返す。
 *
 * @details
 * 将来 InviteFriend で「ローカル list 内のフレンドのみ許可」等のポリシーが必要に
 * なったときに使う未使用ヘルパ。dead-code 警告回避のため [[maybe_unused]] を付ける。
 * @param list 走査するフレンドリスト。
 * @param platform_id 探す platform_id。
 * @return 含まれていれば true (platform_id == nullptr は false)。
 */
[[maybe_unused]] static bool FriendListContains(const TArray<FFriend>& list,
                                                const char* platform_id) noexcept {
    if (platform_id == nullptr) return false;
    const usize n = list.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(list[i].platform_id, platform_id)) return true;
    }
    return false;
}

} // namespace acs::game
