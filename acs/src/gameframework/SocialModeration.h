// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 通報カテゴリ。
 *
 * @details
 * プラットフォーム規約 (Steam / PSN / Xbox / NSO) で共通的に求められる種別を
 * 最小公倍数として定義する。プラットフォーム固有の細分カテゴリ (Steam の
 * 「不正な広告」等) は OtherToxicity の note に記録する。
 */
enum class EReportCategory : u8 {
    /** 嫌がらせ / 暴言 / つきまとい。 */
    Harassment        = 0,

    /** ヘイトスピーチ / 差別表現。 */
    HateSpeech        = 1,

    /** チート / マクロ / 不正ツール。 */
    Cheating          = 2,

    /** スパム / 広告 / 詐欺勧誘。 */
    Spam              = 3,

    /** 不適切な名前 / アバター。 */
    InappropriateName = 4,

    /** その他 (note で詳細記述)。 */
    OtherToxicity     = 5,
};

/**
 * 通報 1 件分のレコード。
 *
 * @details
 * `reported_user_id` / `reporter_user_id` / `note` はすべて const char* 非所有で、
 * 呼び出し側が操作中の寿命を保証する。timestamp は Unix 秒など呼び出し側が決めた
 * 単調増加値で、本 system は比較せず保存のみ行う。
 */
struct FReportRecord {
    /** 通報対象の user_id (SDK 固有 ID、非所有)。 */
    const char*    reported_user_id = nullptr;

    /** 通報者の user_id (通常 local player、非所有)。 */
    const char*    reporter_user_id = nullptr;

    /** 通報カテゴリ (種別選択は規約上必須)。 */
    EReportCategory category         = EReportCategory::OtherToxicity;

    /** 自由記述 (空文字 or nullptr 可、非所有)。 */
    const char*    note             = nullptr;

    /** 通報時刻 (呼び出し側基準の単調増加値)。 */
    u64            timestamp        = 0;
};

/**
 * ブロックリスト 1 件分のエントリ。
 *
 * @details
 * `blocked_user_id` は const char* 非所有。timestamp は監査ログ用
 * (「いつブロックしたか」UI 表示) で、本 system は比較しない。
 */
struct FBlockEntry {
    /** ブロック対象の user_id (SDK 固有 ID、非所有)。 */
    const char* blocked_user_id = nullptr;

    /** ブロック時刻 (呼び出し側基準の値)。 */
    u64         timestamp       = 0;
};

/**
 * ローカルブロックリスト管理 + 通報キュー。
 *
 * @details
 * ブロックリストの追加 / 解除 / 検索と通報キューへの追加 / フラッシュはすべて
 * ローカルで完結し、実プラットフォームのモデレーション SDK (ReportPlayer 等) への
 * 送信は seam として未接続。通常は長寿命 1 個で運用し、誤コピーで block list が
 * 分裂して安全性が損なわれるのを避けるため非コピー・非ムーブとする。
 */
class CSocialModeration {
public:
    /** 空状態で構築する。 */
    CSocialModeration()  noexcept = default;

    /** 破棄する。 */
    ~CSocialModeration() noexcept = default;

    /** コピー禁止 (block list の分裂を防ぐため)。 */
    CSocialModeration(const CSocialModeration&)            = delete;

    /** コピー代入も禁止。 */
    CSocialModeration& operator=(const CSocialModeration&) = delete;

    /** ムーブ禁止 (block list の分裂を防ぐため)。 */
    CSocialModeration(CSocialModeration&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSocialModeration& operator=(CSocialModeration&&)      = delete;

    /**
     * 内部 state を初期化する。
     *
     * @details 内部状態を持たない no-op で、多重呼び出しできる。
     */
    void Init() noexcept;

    /**
     * user_id をローカルブロックリストに追加する。
     *
     * @details 既に登録済み、または user_id == nullptr なら no-op。
     * @param user_id ブロックする相手の user_id (非所有)。
     */
    void BlockUser(const char* user_id) noexcept;

    /**
     * user_id をローカルブロックリストから削除する。
     *
     * @details 未登録なら no-op。順序は保持しない (RemoveAtSwap)。
     * @param user_id ブロック解除する相手の user_id (非所有)。
     */
    void UnblockUser(const char* user_id) noexcept;

    /**
     * user_id がブロック済みかを返す。
     *
     * @details ブロック済みユーザーへの処理可否を判断する。
     * @param user_id 判定する相手の user_id (nullptr は常に false)。
     * @return ブロック済みなら true。
     */
    bool IsBlocked(const char* user_id) const noexcept;

    /**
     * ブロック件数を返す。
     *
     * @return 現在のブロックリストの件数。
     */
    u32 BlockedCount() const noexcept;

    /**
     * ブロックリストの生バッファを返す。
     *
     * @details BlockUser / UnblockUser / ClearLocalState で無効化される。
     * @param out_count 件数 (BlockedCount() と同値) を受け取る参照。
     * @return 先頭要素へのポインタ (out_count 件、空なら実装依存)。
     */
    const FBlockEntry* AllBlocked(u32& out_count) const noexcept;

    /**
     * 通報を送信する。
     *
     * @details
     * 通報を pending queue に追加して Ok() を返す。
     * reported_user_id == nullptr は弾く。
     * @param rep 送信する通報レコード。
     * @return 受理または queue 追加に成功すれば Ok、不正入力ならエラー。
     */
    TResult<void> SubmitReport(const FReportRecord& rep) noexcept;

    /**
     * 未送信通報の件数を返す。
     *
     * @return pending queue のサイズ。
     */
    u32 PendingReportCount() const noexcept;

    /**
     * これまでに backend へ受理された通報の累計件数を返す。
     *
     * @details
     * FlushReports / SubmitReport が seam 経由で受理に成功した分だけ加算される。
     * ClearLocalState でも 0 にリセットしない (アカウント単位の累計を保つ)。
     * @return 受理済み通報の累計件数。
     */
    u32 DeliveredReportCount() const noexcept;

    /**
     * backend (ISteamworksBridge / cloud filter) への接続有無を切り替える seam。
     *
     * @details
     * 上位レイヤが SDK 接続完了 / 切断時に呼ぶ。false の間は FlushReports が
     * 1 件も受理せず queue を保持する (「送信したつもりで消える」事故を防ぐ)。
     * @param connected backend が接続済みなら true。
     */
    void SetBackendConnected(bool connected) noexcept;

    /**
     * backend が接続済みかを返す。
     *
     * @return 接続済みなら true。
     */
    bool IsBackendConnected() const noexcept;

    /**
     * 未送信通報をまとめて backend に送信する。
     *
     * @details
     * queue 先頭から TrySubmitToBackend() を順次呼び、受理された分だけ queue から
     * 削除して m_Delivered を加算、未受理 (= backend 未接続) の分は queue に残す。
     * @return 全件受理 or 元から空なら Ok、1 件でも残れば集約エラー。
     */
    TResult<void> FlushReports() noexcept;

    /**
     * ブロックリストと通報キューを両方クリアする。
     *
     * @details テスト用 / セーブデータ削除 / アカウント切り替え時に使う。SDK 同期は行わない。
     */
    void ClearLocalState() noexcept;

private:
    /**
     * user_id が既にブロックリストにあるかを線形走査で判定する。
     *
     * @param user_id 検索する user_id (nullptr は false)。
     * @return 見つかれば true。
     */
    bool FindBlocked(const char* user_id) const noexcept;

    /**
     * 1 件の通報を backend へ送信する seam。
     *
     * @details
     * 接続済み backend へのネットワーク送信本体を実装する。m_BackendConnected が false の間は
     * 常に false を返す (1 件も受理せず queue に残す)。local state machine はこの戻り値だけを
     * 見て bookkeeping し、SDK の有無を意識しない。
     * @param rep 送信する通報レコード。
     * @return backend に受理されたら true。
     */
    bool TrySubmitToBackend(const FReportRecord& rep) noexcept;

    /** ローカルブロックリスト。 */
    TArray<FBlockEntry>   m_Blocked;

    /** 未送信通報キュー。 */
    TArray<FReportRecord> m_PendingReports;

    /** backend 接続フラグ (seam)。 */
    bool                 m_BackendConnected = false;

    /** backend 受理済み通報の累計件数。 */
    u32                  m_Delivered        = 0;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSocialModeration = CSocialModeration;

} // namespace acs::game
