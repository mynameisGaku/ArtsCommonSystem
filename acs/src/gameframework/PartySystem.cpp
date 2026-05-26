// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — FPartySystem 実装 (Phase T-1 スケルトン)
//
// 現フェーズ: state machine + メンバ/フレンドリスト管理は完全実装。実 SDK 呼び出し
// (SteamworksBridge / EOS / PSN / Xbox / NSO) は seam として未接続で、Joining /
// Leaving は Tick で仮想完了する。これにより呼び出し側 (FGame / FScene) は本 system を
// 通常通り使い始めることができ、Pillar S 実装到着時に bridge を差し替えるだけで実
// プラットフォーム動作する設計。
//
// 設計メモ:
//   ・SDK 完了 (kPendingDurationSec 経過) は **常に成功扱い** にしている。実 SDK
//     接続後は Tick 中に bridge.PollResult() を見て成功/失敗を分岐する。失敗時は
//     state を Solo に戻すリセット動作を入れる予定 (現スケルトンでは未実装)。
//   ・タイムアウト閾値は短め (1.5 秒) に設定。これは UI のスピナー演出を兼ねた
//     疑似遅延であり、実 SDK 接続後は外部の真の completion event に置き換わる。
//   ・メンバ / フレンド比較は Pillar O Entitlement と同じく自前 StrEq で実施。
//     STL 禁止 + <cstring> も避ける方針。
#include "gameframework/PartySystem.h"

namespace acs::game {

namespace {

// 仮想 SDK 完了までの待ち時間 (秒)。Phase T-1 スケルトンの暫定値。
// Phase T-2 で real bridge に差し替え後は使用しない。
constexpr f32 kPendingDurationSec = 1.5f;

// const char* の安全比較 (Pillar O Entitlement と同じ実装)。どちらかが nullptr
// なら false。終端ヌルまで一致比較。
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

void FPartySystem::EmplaceSelfAsLeader() noexcept {
    // Phase T-2 で SteamworksBridge::GetLocalPlayerId() / GetLocalDisplayName()
    // から取得して入れ替える。現スケルトンでは固定文字列リテラル (寿命無限) を使用。
    FPartyMember self{};
    self.player_id    = "local:self";
    self.display_name = "You";
    self.is_leader    = true;
    self.is_ready     = false;
    _members.PushBack(self);
}

TResult<void> FPartySystem::CreateParty(const char* party_name) noexcept {
    if (_state != EPartyState::Solo) {
        // 既にパーティ所属中 / 状態遷移中での再要求は許可しない。先に LeaveParty を
        // 完了させること。Generic + subcode 1 = "invalid state for CreateParty"。
        return ACS_ERR(Generic, 1, "FPartySystem::CreateParty: not in Solo state");
    }
    if (party_name == nullptr) {
        // 空名は将来 SDK によっては許可するが、bridge 統一のため最低限名前必須。
        return ACS_ERR(Generic, 2, "FPartySystem::CreateParty: party_name is null");
    }
    _party_name    = party_name;
    _party_id      = nullptr;  // 自分作成時は SDK が後で割り当てる想定
    _pending_timer = 0.0f;
    _state         = EPartyState::Joining;
    // TODO(Phase T-2): SteamworksBridge.CreateLobby(party_name) を発行し、
    //   Tick() 中に PollResult を見て InParty 遷移する。失敗時は Solo に戻す。
    return Ok();
}

TResult<void> FPartySystem::JoinParty(const char* party_id) noexcept {
    if (_state != EPartyState::Solo) {
        return ACS_ERR(Generic, 3, "FPartySystem::JoinParty: not in Solo state");
    }
    if (party_id == nullptr) {
        return ACS_ERR(Generic, 4, "FPartySystem::JoinParty: party_id is null");
    }
    _party_name    = nullptr;
    _party_id      = party_id;
    _pending_timer = 0.0f;
    _state         = EPartyState::Joining;
    // TODO(Phase T-2): SteamworksBridge.JoinLobby(party_id) を発行。
    return Ok();
}

TResult<void> FPartySystem::LeaveParty() noexcept {
    if (_state != EPartyState::InParty) {
        // Joining / Leaving / Solo からの離脱は no-op ではなくエラーで返す
        // (上位レイヤで状態整合を取れていない時に気付けるように)。
        return ACS_ERR(Generic, 5, "FPartySystem::LeaveParty: not in InParty state");
    }
    _pending_timer = 0.0f;
    _state         = EPartyState::Leaving;
    // TODO(Phase T-2): SteamworksBridge.LeaveLobby(_party_id) を発行。
    return Ok();
}

TResult<void> FPartySystem::InviteFriend(const char* friend_id) noexcept {
    if (_state != EPartyState::InParty) {
        return ACS_ERR(Generic, 6, "FPartySystem::InviteFriend: not in InParty state");
    }
    if (friend_id == nullptr) {
        return ACS_ERR(Generic, 7, "FPartySystem::InviteFriend: friend_id is null");
    }
    // フレンドリスト内に存在するかは「警告のみ」で弾かない。プラットフォーム
    // 上は friend でない相手にも (公開設定次第で) invite を送れるケースがあり、
    // 厳格にローカル list と突き合わせると有効な招待を弾く可能性があるため。
    // 一方で moderation / blocking 判定はここでは行わない (将来の別モジュール)。
    (void)friend_id;
    // TODO(Phase T-2): SteamworksBridge.InviteFriendToLobby(_party_id, friend_id)。
    //   結果通知 (送信失敗 / 受信側拒否 / 受信側 accept) は bridge イベントで
    //   非同期に流れてくる想定。本 API は「送信できたか」だけを返す。
    return Ok();
}

u32 FPartySystem::MemberCount() const noexcept {
    return static_cast<u32>(_members.Size());
}

const FPartyMember* FPartySystem::Members() const noexcept {
    return _members.Data();
}

void FPartySystem::Tick(f32 dt) noexcept {
    // Solo / InParty では時間進行は不要 (将来 idle 検出を入れる場合はここを拡張)。
    if (_state == EPartyState::Joining) {
        _pending_timer += dt;
        if (_pending_timer >= kPendingDurationSec) {
            // 仮想 SDK 完了: メンバリストに自分をリーダーとして登録、InParty へ。
            // 既存メンバが残っている可能性は通常無いが、防御的に Clear してから登録。
            _members.Clear();
            EmplaceSelfAsLeader();
            _pending_timer = 0.0f;
            _state         = EPartyState::InParty;
        }
        return;
    }
    if (_state == EPartyState::Leaving) {
        _pending_timer += dt;
        if (_pending_timer >= kPendingDurationSec) {
            // 仮想 SDK 完了: メンバリストをクリアして Solo に戻す。
            _members.Clear();
            _party_name    = nullptr;
            _party_id      = nullptr;
            _pending_timer = 0.0f;
            _state         = EPartyState::Solo;
        }
        return;
    }
}

void FPartySystem::AddFriend(const FFriend& f) noexcept {
    if (f.platform_id == nullptr) {
        // SDK 取得失敗時の nullptr 流入で list を壊さない (Pillar O と同じ防御)。
        return;
    }
    // 重複 platform_id は上書きせず単純追加 (Pillar S 側で dedup する想定)。
    // 必要なら同 ID 検出ループを将来追加するが、線形走査コストを増やすので保留。
    _friends.PushBack(f);
}

u32 FPartySystem::FriendCount() const noexcept {
    // Pillar O Entitlement と同じく u32 範囲で十分 (現実的に friend list は
    // SDK 制限内: 大規模 SNS の Steam で 2000、PSN で 2000、Xbox で 1000 等)。
    return static_cast<u32>(_friends.Size());
}

const FFriend* FPartySystem::Friends() const noexcept {
    return _friends.Data();
}

// 未使用ヘルパだが、将来 InviteFriend で「ローカル list 内のフレンドのみ許可」
// 等のポリシーが必要になったときに使う。コンパイル時の dead-code 警告を避ける
// ため、明示的に [[maybe_unused]] でマーク。
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
