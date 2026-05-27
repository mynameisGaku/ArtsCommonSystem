// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — FSocialModeration (blocking / reporting / 通報管理)
//
// 役割:
//   ローカルブロックリストと通報 (報告) キューを管理する。実プラットフォームの
//   モデレーション SDK (Steam ISteamUser::ReportPlayer / EOS ReportPlayer /
//   PSN Communication Block / Xbox Reputation / NSO 通報 API) への送信は seam
//   として未接続で、Phase T-3 以降で `FSteamworksBridge` 等を経由して接続する。
//
//   FPartySystem.h で「moderation / blocking は別モジュール」と明記した通り、
//   本 system が「上位レイヤから呼ばれる単一窓口」を担う。InviteFriend で
//   ブロック相手かどうかを判定したい場合は、呼び出し側で IsBlocked() を
//   先に問い合わせる責任分離 (FPartySystem 自身は moderation を意識しない)。
//
// 設計上の倫理方針 (通報 / ブロック + 児童保護):
//   ・**ブロックはローカル即時反映**: BlockUser はネットワーク往復を伴わず、
//     UI 上「ブロックしました」を即座に確定できる。SDK 連携 (相互通信遮断)
//     は seam 経由で後追い同期する想定。これにより通信障害時でもユーザーが
//     「ブロックしたつもりで通信が続く」事故を防ぐ。
//   ・**通報は category 必須**: 自由記述だけだと審査側で分類困難。プラット
//     フォーム規約 (Steam / PSN / Xbox / NSO のすべて) で「種別選択」が
//     共通要件となっており、enum で型安全に強制する。
//   ・**通報送信失敗時は queue に残す**: ネットワーク不安定環境 (モバイル
//     ゲーム / 機内 Wi-Fi) で「通報したつもりが消えた」を防ぐため、送信
//     失敗時は pending queue に保持し FlushReports() で再送する設計。
//   ・**under-18 デフォルト**: 未成年アカウントの場合は呼び出し側で
//     「全ユーザーをデフォルトブロック / フレンドのみ可」等のポリシーを
//     かぶせる想定。本 system はフラグを持たず、強制機構も入れない
//     (プラットフォームごとの年齢推定 API 差を吸収するため、上位レイヤで
//     判断する責任分離)。
//   ・**自分自身のブロックは防御的に弾かない**: 文字列比較で「自分の
//     user_id」を知らないため、上位レイヤで弾く責任。本 system は受け取った
//     文字列をそのままリストに入れる (FPartySystem と同じ哲学)。
//
// 使い方 (典型例):
//   FSocialModeration mod;
//   mod.Init();
//
//   // toxic プレイヤーを即時ブロック
//   mod.BlockUser("steam:76561198000000999");
//
//   // 通報送信
//   ReportRecord rep{};
//   rep.reported_user_id = "steam:76561198000000999";
//   rep.reporter_user_id = "steam:76561198000000001";  // local player
//   rep.category         = EReportCategory::Harassment;
//   rep.note             = "voice chat で継続的に侮辱発言";
//   rep.timestamp        = NowUnixSec();
//   (void)mod.SubmitReport(rep);  // 失敗時は pending queue へ
//
//   // 後で再送 (オンライン復帰時など)
//   (void)mod.FlushReports();
//
// 設計選択 (Phase T-2 スケルトン):
//   ・**ローカル state は完全実装**: ブロックリストの追加/解除/検索、通報
//     キューへの追加/フラッシュはすべて動く。
//   ・**SDK 接続は TODO**: 実 ReportPlayer 送信 / Steam Block 同期 / PSN
//     CommunicationRestriction 反映は Phase T-3 以降で接続。本 system は
//     seam として const char* (user_id) を受けるだけ。
//   ・**const char* 非所有**: 規約通り <string> 不使用。user_id / note の
//     寿命は呼び出し側 (文字列リテラル or 長寿命バッファ) が保証する。
//     SDK 側で動的取得した名前は呼び出し側で永続バッファにコピーして渡す。
//   ・**重複ブロックは no-op**: 既にブロック済みの user_id を再度 BlockUser
//     しても list が肥大化しない (検索 → 早期 return)。
//   ・**コピー / ムーブ禁止**: モデレーション state は通常 1 つの長寿命
//     オブジェクトで運用。誤コピーで block list が分裂すると安全性が
//     損なわれるため非コピー・非ムーブ。
//   ・**全 noexcept**: ACS 全体方針 (TResult<T, FErrorCode> + bool 戻り値)。
//
// 範囲外 (Phase T-3 以降):
//   ・実 SDK 接続 (Steam ReportPlayer / EOS / PSN / Xbox Reputation / NSO)
//   ・ブロック list の永続化 (Pillar J Serialize 経由 / クラウド同期)
//   ・自動モデレーション (toxic 検出 ML、Pillar U AI 側に分離)
//   ・voice / text chat の透過フィルタ (別モジュール)
//   ・shadow-ban / mute 区別 (今は block のみ)
//   ・freezing period / クールダウン (連続通報の rate limit)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

// 通報カテゴリ。プラットフォーム規約 (Steam / PSN / Xbox / NSO) で共通的に
// 求められる種別を最小公倍数として定義。プラットフォーム固有の細分カテゴリ
// (Steam の「不正な広告」等) は将来 OtherToxicity 内の note で表現する想定。
enum class EReportCategory : u8 {
    Harassment        = 0,  // 嫌がらせ / 暴言 / つきまとい
    HateSpeech        = 1,  // ヘイトスピーチ / 差別表現
    Cheating          = 2,  // チート / マクロ / 不正ツール
    Spam              = 3,  // スパム / 広告 / 詐欺勧誘
    InappropriateName = 4,  // 不適切な名前 / アバター
    OtherToxicity     = 5,  // その他 (note で詳細記述)
};

// 通報 1 件分。`reported_user_id` / `reporter_user_id` / `note` はすべて
// const char* 非所有 (FPartySystem と同じポリシー)。timestamp は Unix 秒など
// 呼び出し側が決めた単調増加値 (本 system は比較せず保存のみ)。
struct ReportRecord {
    const char*    reported_user_id = nullptr;  // 通報対象 (SDK 固有 ID)
    const char*    reporter_user_id = nullptr;  // 通報者 (通常 local player)
    EReportCategory category         = EReportCategory::OtherToxicity;
    const char*    note             = nullptr;  // 自由記述 (空文字 or nullptr 可)
    u64            timestamp        = 0;        // 通報時刻 (呼び出し側基準)
};

// ブロックリスト 1 件分。`blocked_user_id` は const char* 非所有。
// timestamp は監査ログ用 (「いつブロックしたか」UI 表示) で本 system は比較しない。
struct FBlockEntry {
    const char* blocked_user_id = nullptr;  // ブロック対象 (SDK 固有 ID)
    u64         timestamp       = 0;        // ブロック時刻 (呼び出し側基準)
};

// ローカルブロックリスト管理 + 通報キュー。
class FSocialModeration {
public:
    FSocialModeration()  noexcept = default;
    ~FSocialModeration() noexcept = default;

    // 通常は長寿命 1 個運用。誤コピーで block list が分裂して安全性が
    // 損なわれるのを避けるため非コピー・非ムーブ。
    FSocialModeration(const FSocialModeration&)            = delete;
    FSocialModeration& operator=(const FSocialModeration&) = delete;
    FSocialModeration(FSocialModeration&&)                 = delete;
    FSocialModeration& operator=(FSocialModeration&&)      = delete;

    // ----- 初期化 -----
    // 内部 state を初期化 (現フェーズでは no-op、将来 SDK ハンドルや永続化
    // ロードを行う想定の seam)。多重呼び出し可。
    void Init() noexcept;

    // ----- block list 操作 -----
    // user_id をローカルブロックリストに追加。既に登録済みなら no-op。
    // user_id == nullptr も no-op (FPartySystem.AddFriend と同じ防御)。
    // SDK 同期は TODO (Phase T-3 で FSteamworksBridge.SetCommunicationRestriction)。
    void BlockUser(const char* user_id) noexcept;

    // user_id をローカルブロックリストから削除。未登録なら no-op。
    // 順序は保持しない (RemoveAtSwap)。SDK 同期は同上 TODO。
    void UnblockUser(const char* user_id) noexcept;

    // user_id がブロック済みか。nullptr は常に false。
    // FPartySystem.InviteFriend() の前段ガードとしての呼び出しを想定。
    bool IsBlocked(const char* user_id) const noexcept;

    // ブロック件数。
    u32 BlockedCount() const noexcept;

    // ブロックリスト生バッファ (BlockedCount() 件)。BlockUser / UnblockUser /
    // ClearLocalState で無効化される。out_count に件数も返す (利便性)。
    const FBlockEntry* AllBlocked(u32& out_count) const noexcept;

    // ----- 通報 -----
    // 通報を送信する。現フェーズでは SDK 未接続のため常に pending queue に
    // 追加して Ok() を返す (将来 backend 接続後は同期送信を試みて、失敗時
    // のみ queue に残す挙動になる)。reported_user_id == nullptr は弾く。
    TResult<void> SubmitReport(const ReportRecord& rep) noexcept;

    // 未送信通報の件数 (queue サイズ)。
    u32 PendingReportCount() const noexcept;

    // 未送信通報をまとめて backend に送信する seam。現フェーズでは SDK 未接続
    // のため queue を空にして Ok() を返す (実 SDK 接続後は ReportPlayer を
    // 各件に対して呼び、失敗時は残す)。
    TResult<void> FlushReports() noexcept;

    // ----- 全消去 -----
    // ブロックリストと通報キューを両方クリアする。テスト用 / セーブデータ
    // 削除 / アカウント切り替え時に使用。SDK 同期は行わない (ローカルのみ)。
    void ClearLocalState() noexcept;

private:
    // 重複ブロック検査用ヘルパ。線形走査 (block list は通常 100 件以下を想定)。
    // 見つかったら true、なければ false。nullptr は false。
    bool FindBlocked(const char* user_id) const noexcept;

    TArray<FBlockEntry>   m_Blocked;        // ローカルブロックリスト
    TArray<ReportRecord> m_PendingReports; // 未送信通報キュー
};

} // namespace acs::game
