// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — SocialModeration 実装 (Phase T-2 スケルトン)
//
// 現フェーズ: ローカルブロックリストと通報キューの管理は完全実装。実 SDK 呼び出し
// (Steam ISteamUser::ReportPlayer / EOS / PSN / Xbox / NSO) は seam として未接続で、
// SubmitReport は queue に積むだけ、FlushReports は queue を空にして成功扱い。
// これにより呼び出し側 (Game / UI) は本 system を通常通り使い始めることができ、
// Pillar S = Storefront 実装到着時に bridge 経由で実プラットフォームに接続する。
//
// 設計メモ:
//   ・ブロック検索は線形走査。block list は通常 100 件以下を想定 (Steam の Block
//     list 上限が 100、PSN も同程度) なので O(n) で十分。仮に 1000 件規模になっても
//     1 フレームあたりの IsBlocked 呼び出し回数は数件 (InviteFriend 直前等) なので
//     ハッシュ化のコストを上回らない。
//   ・通報送信は将来 backend 接続後に「送信成功 → queue から削除」の挙動に変える。
//     現フェーズでは SubmitReport が必ず queue に積み、FlushReports で全件成功扱い。
//   ・文字列比較は PartySystem と同じ StrEq (<cstring> も避ける ACS 規約)。
//   ・空 user_id (nullptr) は防御的に弾く: SDK 取得失敗時の nullptr 流入で list を
//     壊さないため (Pillar O Entitlement / PartySystem.AddFriend と同じポリシー)。
#include "gameframework/SocialModeration.h"

namespace acs::game {

namespace {

// const char* の安全比較 (PartySystem と同じ実装、ACS 規約 <cstring> 不使用)。
// どちらかが nullptr なら false。終端ヌルまで一致比較。
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

void SocialModeration::Init() noexcept {
    // Phase T-3 で永続化された block list のロード / SteamworksBridge への
    // 接続を行う seam。現スケルトンでは no-op (Array はデフォルト初期化済み)。
}

bool SocialModeration::FindBlocked(const char* user_id) const noexcept {
    if (user_id == nullptr) return false;
    const usize n = _blocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_blocked[i].blocked_user_id, user_id)) return true;
    }
    return false;
}

void SocialModeration::BlockUser(const char* user_id) noexcept {
    if (user_id == nullptr) {
        // SDK 取得失敗時の nullptr 流入で list を壊さない (PartySystem.AddFriend と同じ)。
        return;
    }
    if (FindBlocked(user_id)) {
        // 既にブロック済み: 重複追加で list が肥大化するのを避けるため早期 return。
        // タイムスタンプ更新は行わない (最初にブロックした時刻を保持する方が監査
        // ログ的に有用なため)。
        return;
    }
    BlockEntry e{};
    e.blocked_user_id = user_id;
    // timestamp は本 system では取得しない (時刻取得 API への依存を避けるため)。
    // 呼び出し側が必要なら BlockUser 前後で自分で記録する想定だが、構造体に
    // フィールドを残すことで将来の API 拡張余地は確保。
    e.timestamp = 0;
    _blocked.PushBack(e);
    // TODO(Phase T-3): SteamworksBridge.SetCommunicationRestriction(user_id, true)。
    //   PSN / Xbox では SDK 側にも同期反映が必要 (通信遮断のため)。
}

void SocialModeration::UnblockUser(const char* user_id) noexcept {
    if (user_id == nullptr) return;
    const usize n = _blocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_blocked[i].blocked_user_id, user_id)) {
            // 順序は保持しない (RemoveAtSwap)。block list は集合的に扱われるため
            // UI 表示でも順序依存しない設計。
            _blocked.RemoveAtSwap(i);
            // TODO(Phase T-3): SteamworksBridge.SetCommunicationRestriction(user_id, false)。
            return;
        }
    }
    // 未登録の user_id を Unblock しても no-op (UI で「ブロック中だと思っていたが
    // 実は登録されていなかった」ケースで例外を出さない)。
}

bool SocialModeration::IsBlocked(const char* user_id) const noexcept {
    return FindBlocked(user_id);
}

u32 SocialModeration::BlockedCount() const noexcept {
    return static_cast<u32>(_blocked.Size());
}

const BlockEntry* SocialModeration::AllBlocked(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_blocked.Size());
    return _blocked.Data();
}

Result<void> SocialModeration::SubmitReport(const ReportRecord& rep) noexcept {
    if (rep.reported_user_id == nullptr) {
        // 通報対象が空なら審査側で識別不能 (必須項目)。Generic + subcode 1。
        return ACS_ERR(Generic, 1, "SocialModeration::SubmitReport: reported_user_id is null");
    }
    // 通報者 (reporter_user_id) と note は欠落許容: 匿名通報 / 種別のみ通報の
    // ケースをサポート (プラットフォームによっては reporter 非公開で送信可能)。

    // 現フェーズでは SDK 未接続のため queue に積むだけで Ok() を返す。
    // Phase T-3 で SteamworksBridge.ReportPlayer(rep) を呼び、成功時は queue に
    // 積まない / 失敗時のみ積む挙動に変更する予定。
    _pending_reports.PushBack(rep);
    // TODO(Phase T-3): bridge 経由で同期送信を試みる。失敗時のみ queue に残す。
    return Ok();
}

u32 SocialModeration::PendingReportCount() const noexcept {
    return static_cast<u32>(_pending_reports.Size());
}

Result<void> SocialModeration::FlushReports() noexcept {
    // 現フェーズでは SDK 未接続のため queue を空にして Ok() を返す。
    // Phase T-3 で bridge.ReportPlayer(_pending_reports[i]) を順次呼び、
    // 成功した分だけ queue から削除する挙動に変更する。失敗が混在した場合は
    // 部分成功扱い (成功分は削除、失敗分は queue に残す + 集約エラーを返す)。
    _pending_reports.Clear();
    // TODO(Phase T-3): bridge 経由で全件送信、失敗分は queue に残す。
    return Ok();
}

void SocialModeration::ClearLocalState() noexcept {
    // テスト / アカウント切り替え / セーブデータ削除時に呼ぶ。SDK 同期は
    // 行わない (ローカル state のみ消去するため、SDK 側のブロック設定は
    // 別途 UnblockUser を呼ぶか、SDK 自身の管理画面から消す必要がある)。
    _blocked.Clear();
    _pending_reports.Clear();
}

} // namespace acs::game
