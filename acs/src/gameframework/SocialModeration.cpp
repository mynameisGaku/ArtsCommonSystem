// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — CSocialModeration 実装
//
// ローカルブロックリストと通報キューの管理は完全実装。実 SDK 呼び出し
// (Steam ISteamUser::ReportPlayer / EOS / PSN / Xbox / NSO) は seam として未接続で、
// SubmitReport は queue に積むだけ、FlushReports は queue を空にして成功扱い。
// これにより呼び出し側 (CGame / UI) は本 system を通常通り使い始めることができ、
// Pillar S = Storefront 実装到着時に bridge 経由で実プラットフォームに接続する。
//
// 設計メモ:
//   ・ブロック検索は線形走査。block list は通常 100 件以下を想定 (Steam の Block
//     list 上限が 100、PSN も同程度) なので O(n) で十分。仮に 1000 件規模になっても
//     1 フレームあたりの IsBlocked 呼び出し回数は数件 (InviteFriend 直前等) なので
//     ハッシュ化のコストを上回らない。
//   ・通報送信は backend 接続後に「送信成功 → queue から削除」の挙動になる。
//     現状は SubmitReport が必ず queue に積み、FlushReports で全件成功扱い。
//   ・文字列比較は CPartySystem と同じ StrEq (<cstring> も避ける ACS 規約)。
//   ・空 user_id (nullptr) は防御的に弾く: SDK 取得失敗時の nullptr 流入で list を
//     壊さないため (Pillar O FEntitlement / CPartySystem.AddFriend と同じポリシー)。
#include "gameframework/SocialModeration.h"

namespace acs::game {

namespace {

/**
 * const char* を安全に比較する (ACS 規約 <cstring> 不使用)。
 *
 * @details どちらかが nullptr なら false。終端ヌルまで一致比較する。
 * @param a 比較する文字列 A。
 * @param b 比較する文字列 B。
 * @return 両者が等しければ true。
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

/** 永続化 block list のロード / SDK 接続を行う seam (現状は no-op)。 */
void CSocialModeration::Init() noexcept {
    // 永続化された block list のロード / FSteamworksBridge への接続を行う seam。
    // 現状では no-op (TArray はデフォルト初期化済み)。
}

/** block list を線形走査し user_id が含まれるかを返す (nullptr は false)。 */
bool CSocialModeration::FindBlocked(const char* user_id) const noexcept {
    if (user_id == nullptr) return false;
    const usize n = m_Blocked.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Blocked[i].blocked_user_id, user_id)) return true;
    }
    return false;
}

/** user_id をブロック登録する (nullptr / 既登録は no-op)。 */
void CSocialModeration::BlockUser(const char* user_id) noexcept {
    if (user_id == nullptr) {
        // SDK 取得失敗時の nullptr 流入で list を壊さない (CPartySystem.AddFriend と同じ)。
        return;
    }
    if (FindBlocked(user_id)) {
        // 既にブロック済み: 重複追加で list が肥大化するのを避けるため早期 return。
        // タイムスタンプ更新は行わない (最初にブロックした時刻を保持する方が監査
        // ログ的に有用なため)。
        return;
    }
    FBlockEntry e{};
    e.blocked_user_id = user_id;
    // timestamp は本 system では取得しない (時刻取得 API への依存を避けるため)。
    // 呼び出し側が必要なら BlockUser 前後で自分で記録する想定だが、構造体に
    // フィールドを残すことで将来の API 拡張余地は確保。
    e.timestamp = 0;
    m_Blocked.Add(e);
    // SDK 同期 (FSteamworksBridge.SetCommunicationRestriction(user_id, true)) は
    // backend 接続後に行う seam。PSN / Xbox では SDK 側にも通信遮断の反映が必要。
}

/** user_id のブロックを解除する (nullptr / 未登録は no-op、順序非保持)。 */
void CSocialModeration::UnblockUser(const char* user_id) noexcept {
    if (user_id == nullptr) return;
    const usize n = m_Blocked.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Blocked[i].blocked_user_id, user_id)) {
            // 順序は保持しない (RemoveAtSwap)。block list は集合的に扱われるため
            // UI 表示でも順序依存しない設計。
            m_Blocked.RemoveAtSwap(i);
            // SDK 同期 (FSteamworksBridge.SetCommunicationRestriction(user_id, false))
            // は backend 接続後に行う seam。
            return;
        }
    }
    // 未登録の user_id を Unblock しても no-op (UI で「ブロック中だと思っていたが
    // 実は登録されていなかった」ケースで例外を出さない)。
}

/** user_id がブロック済みかを返す (nullptr は false)。 */
bool CSocialModeration::IsBlocked(const char* user_id) const noexcept {
    return FindBlocked(user_id);
}

/** ブロック済みユーザー数を返す。 */
u32 CSocialModeration::BlockedCount() const noexcept {
    return static_cast<u32>(m_Blocked.Num());
}

/** ブロックエントリ配列の先頭を返し、件数を out_count に書き戻す。 */
const FBlockEntry* CSocialModeration::AllBlocked(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Blocked.Num());
    return m_Blocked.GetData();
}

/** 通報を backend へ同期送信する seam (現状は常に未受理 = false を返す)。 */
bool CSocialModeration::TrySubmitToBackend(const FReportRecord& rep) noexcept {
    (void)rep;  // 現状では未使用 (seam の引数だけ確定させておく)。
    // seam: 実ネットワーク送信本体。
    // backend 未接続なら受理しない → 呼び出し側 (SubmitReport / FlushReports) が
    // queue に残し、「送信したつもりで消える」事故を防ぐ。
    // ここに FSteamworksBridge.ReportPlayer(rep) 等を実装し、SDK が
    // 受理 (HTTP 2xx / SDK success) を返したら true を返すように差し替える。
    if (!m_BackendConnected) {
        return false;
    }
    // backend 接続済みでも、実送信ロジック未実装の現状では受理しない。
    // (SetBackendConnected(true) を seam テストで呼んでも、まだ送信本体が無いため
    //  false を返す。これにより「接続フラグだけ立てて中身は空」という嘘の成功を
    //  作らない — 送信実装と同時に true 返却へ差し替える。)
    return false;
}

/** 通報を受け付ける (同期送信を試み、未受理なら pending queue に保持)。 */
TResult<void> CSocialModeration::SubmitReport(const FReportRecord& rep) noexcept {
    if (rep.reported_user_id == nullptr) {
        // 通報対象が空なら審査側で識別不能 (必須項目)。Generic + subcode 1。
        return ACS_ERR(Generic, 1, "CSocialModeration::SubmitReport: reported_user_id is null");
    }
    // 通報者 (reporter_user_id) と note は欠落許容: 匿名通報 / 種別のみ通報の
    // ケースをサポート (プラットフォームによっては reporter 非公開で送信可能)。

    // まず seam 経由で同期送信を試みる。受理されれば queue に積まず累計のみ加算。
    // 未受理 (現状は常にこちら) なら pending queue に保持し、後で
    // FlushReports() による再送に委ねる (オフライン耐性)。どちらの経路でも
    // 「通報を受け付けた」ことは保証されるため呼び出し側には Ok() を返す。
    if (TrySubmitToBackend(rep)) {
        ++m_Delivered;
    } else {
        m_PendingReports.Add(rep);
    }
    return Ok();
}

/** 未送信の保留通報数を返す。 */
u32 CSocialModeration::PendingReportCount() const noexcept {
    return static_cast<u32>(m_PendingReports.Num());
}

/** これまでに送信受理された通報の累計数を返す。 */
u32 CSocialModeration::DeliveredReportCount() const noexcept {
    return m_Delivered;
}

/** backend 接続状態を切り替える seam (自動フラッシュはしない)。 */
void CSocialModeration::SetBackendConnected(bool connected) noexcept {
    // Pillar S 側が SDK 接続完了 / 切断時に呼ぶ seam。接続状態を切り替えるだけで
    // 自動フラッシュはしない (フラッシュ契機は呼び出し側が FlushReports で制御)。
    m_BackendConnected = connected;
}

/** backend が接続済みかを返す。 */
bool CSocialModeration::IsBackendConnected() const noexcept {
    return m_BackendConnected;
}

/** 保留通報を順に再送し、残件があれば集約エラーを返す (順序保持の安定圧縮)。 */
TResult<void> CSocialModeration::FlushReports() noexcept {
    // 未送信通報を queue 先頭から順に seam へ流す local state machine。
    // 受理された分だけ前方に詰め直さず「残す側」を後ろから前へ走査して
    // RemoveAtSwap で抜く設計だと順序が崩れ、通報の時系列が乱れる。通報は
    // timestamp 昇順 (= 追加順) を保ったまま再送したいので、前から走査しつつ
    // 受理分をスキップして「残す分」を in-place で前詰めする安定圧縮を行う。
    const usize n     = m_PendingReports.Num();
    usize       keep  = 0;   // 残す要素の書き込み先 (前詰めカーソル)
    u32         failed = 0;  // 未受理 (= queue に残った) 件数

    for (usize i = 0; i < n; ++i) {
        if (TrySubmitToBackend(m_PendingReports[i])) {
            // 受理: queue から落とす (前詰めしない) + 累計加算。
            ++m_Delivered;
        } else {
            // 未受理: 順序を保って前方へ詰め直す。
            if (keep != i) {
                m_PendingReports[keep] = m_PendingReports[i];
            }
            ++keep;
            ++failed;
        }
    }
    // 末尾の余剰 (受理して落とした分) を捨てて queue サイズを残存数に縮める。
    m_PendingReports.SetNum(keep);

    if (failed != 0) {
        // 1 件でも残れば「全件は送れていない」ことを集約エラーで通知。
        // 呼び出し側はオンライン復帰後に再度 FlushReports を呼ぶ契機にできる。
        return ACS_ERR(Generic, 2,
                       "CSocialModeration::FlushReports: backend not connected; reports remain queued");
    }
    return Ok();
}

/** ローカル state (block list / pending queue) を消去する (累計値・接続状態は保持)。 */
void CSocialModeration::ClearLocalState() noexcept {
    // テスト / アカウント切り替え / セーブデータ削除時に呼ぶ。SDK 同期は
    // 行わない (ローカル state のみ消去するため、SDK 側のブロック設定は
    // 別途 UnblockUser を呼ぶか、SDK 自身の管理画面から消す必要がある)。
    m_Blocked.Reset();
    m_PendingReports.Reset();
    // m_Delivered は累計監査値なのでリセットしない。m_BackendConnected も接続状態
    // を表す seam フラグであり local state ではないため触らない。
}

} // namespace acs::game
