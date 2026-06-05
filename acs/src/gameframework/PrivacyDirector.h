// SPDX-License-Identifier: Apache-2.0
// GameFramework メタ層 — FPrivacyDirector (GDPR / CCPA consent ハブ)
//
// 役割:
//   ゲーム内で扱う個人情報・解析・広告・テレメトリといった
//   「ユーザーの同意」が必要な機能を、bit flag で表す EConsentCategory に
//   集約して一元管理する director。GDPR (欧州) / CCPA (カリフォルニア) /
//   COPPA (児童) 等への対応で必須となる「事前同意の取得」「カテゴリ別の
//   on/off」「ポリシー版の追跡」「永続化」を共通基盤として提供する。
//
// 想定する典型フロー:
//   FPrivacyDirector privacy;
//   privacy.Init(/*current_policy_version=*/2);
//   privacy.LoadConsent(L"user/consent.bin");                 // 既存設定があれば復元
//
//   if (privacy.RequiresInitialConsent() || privacy.IsPolicyOutdated()) {
//       // 同意ダイアログを出して、ユーザーの選択を反映する。
//       privacy.GrantConsent(EConsentCategory::Analytics);
//       privacy.RevokeConsent(EConsentCategory::Marketing);
//       privacy.MarkInitialConsentShown();
//       privacy.SaveConsent(L"user/consent.bin");
//   }
//
//   if (privacy.HasConsent(EConsentCategory::Analytics)) {
//       AnalyticsBackend::SendEvent(...);
//   }
//
// 設計選択:
//   ・**bit flag 中心**: FSceneServices と同じく EConsentCategory を bit OR で
//     合成する設計。「Analytics は ON だが Marketing は OFF」のような
//     カテゴリ別 on/off が必須要件 (GDPR の granular consent) なので、
//     enum class : u32 + operator|/& を提供する。
//   ・**Required (= 0)**: 法的に必須 = consent 不要のサービス
//     (= ローカルセーブ / クラッシュ復帰用 minimum) を表す特異値。
//     HasConsent(Required) は常に true を返す。
//   ・**初回ダイアログ flag**: GDPR は「同意取得前に追跡を始めない」必要が
//     あるため、まだダイアログを出していないかどうかを別 flag で持つ。
//     granted_mask == 0 と「ダイアログ未表示」は区別される
//     (ユーザーが全部 reject した場合は mask = 0 だがダイアログは済んでいる)。
//   ・**policy_version 追跡**: プライバシーポリシーの文言が変わると
//     再同意が必要になる (GDPR Article 7 規定)。Init() に渡した値を
//     "current" として保持し、保存側 stored < current なら IsPolicyOutdated()
//     が true を返して再同意を促す。
//   ・**Save/Load = ローカル永続化**: FSaveSlot<ConsentStatus> 経由で端末ローカル
//     に `.acssave` バイナリ (24B header + payload + CRC32) として書き出す。
//     GDPR/CCPA の同意情報は端末ローカルに保持する性質のものなので、プラット
//     フォーム backend には依存しない (クラウド同期は将来の別案件)。
//   ・**非コピー・非ムーブ**: アプリ全体で 1 個運用される director なので、
//     誤って値渡しされて consent 状態が分裂しないよう移動コンストラクタも禁止。
//   ・**全 noexcept**: ACS 規約に従い例外なし。TResult<void, FErrorCode> で
//     エラーを伝搬する。
//
// 範囲外:
//   ・consent 設定のクラウド同期 (端末間で同意状態を共有する Backend 連携)
//   ・consent 取得 UI そのもの (ゲーム側 / Editor 側で別途実装)
//   ・地域判定 (GDPR 地域だけダイアログを強制する 等の policy)
//   ・consent 変更時の subscribe コールバック (今は pull 型のみ)
//   ・Data Subject Access Request (DSAR): ユーザーが「自分のデータを見せろ」
//     と要求した場合のエクスポート (Pillar V Backend Services 側で扱う想定)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * bit flag で表す同意カテゴリ。
 *
 * @details
 * 各カテゴリは独立に on/off できる (GDPR granular consent)。bit OR で複合できるよう
 * enum class : u32 とし、operator| / operator& を提供する。Required (= 0) は
 * 「同意不要 = 常に許可」を表す特異値で、ゲームの基本動作に必要なサービス
 * (ローカルセーブ等) を分類するためのマーカ。HasConsent() に Required を渡した場合は常に true。
 */
enum class EConsentCategory : u32 {
    /** 法的同意不要 (常に許可される基本機能)。 */
    Required          = 0,

    /** ゲームプレイ統計 / 行動ログ。 */
    Analytics         = 1u << 0,

    /** 広告 / 販促 / プッシュ通知。 */
    Marketing         = 1u << 1,

    /** 個人向けレコメンド / カスタマイズ。 */
    Personalization   = 1u << 2,

    /** SDK / 外部パートナーへのデータ共有。 */
    ThirdPartySharing = 1u << 3,

    /** パフォーマンス計測 / 匿名診断。 */
    Telemetry         = 1u << 4,

    /** クラッシュ収集 (個別 opt-out 用)。 */
    CrashReports      = 1u << 5,
};

/**
 * 2 つの同意カテゴリの bit OR を返す。
 *
 * @param a 左辺のカテゴリ。
 * @param b 右辺のカテゴリ。
 * @return a と b の bit を合成したカテゴリ。
 */
constexpr EConsentCategory operator|(EConsentCategory a, EConsentCategory b) noexcept {
    return static_cast<EConsentCategory>(static_cast<u32>(a) | static_cast<u32>(b));
}

/**
 * 2 つの同意カテゴリの bit AND を返す。
 *
 * @param a 左辺のカテゴリ。
 * @param b 右辺のカテゴリ。
 * @return a と b に共通する bit のみを持つカテゴリ。
 */
constexpr EConsentCategory operator&(EConsentCategory a, EConsentCategory b) noexcept {
    return static_cast<EConsentCategory>(static_cast<u32>(a) & static_cast<u32>(b));
}

/**
 * 永続化単位の POD な同意状態。
 *
 * @details
 * SaveConsent / LoadConsent で書き出される最小構造体。trivially-copyable な POD として
 * 扱えるよう member は全て値型のみ。policy_version は再同意判定の主軸で、
 * consent_timestamp は監査ログ向けに「いつ同意したか」を保持する。
 */
struct ConsentStatus {
    /** bit OR された同意 mask (既定は Required のみ)。 */
    EConsentCategory granted_mask      = EConsentCategory::Required;

    /** 最後に同意した時刻 (epoch ms 等)。 */
    u64             consent_timestamp = 0;

    /** 同意時のポリシー版。 */
    u32             policy_version    = 0;
};

/**
 * GDPR / CCPA の consent 管理ハブ。
 *
 * @details
 * 個人情報・解析・広告・テレメトリなど同意が必要な機能を EConsentCategory に集約して
 * 一元管理する。事前同意の取得・カテゴリ別 on/off・ポリシー版の追跡・ローカル永続化を
 * 共通基盤として提供する。アプリ全体で 1 個運用される想定で、consent 状態が分裂しない
 * よう non-copy / non-move。
 */
class FPrivacyDirector {
public:
    /** 未初期化状態で構築する (Init を呼ぶまで mask は Required のみ)。 */
    FPrivacyDirector()  noexcept = default;

    /** 破棄する (保持するのは POD のみで特別な解放処理はない)。 */
    ~FPrivacyDirector() noexcept = default;

    /** コピー禁止 (consent 状態の分裂を防ぐため)。 */
    FPrivacyDirector(const FPrivacyDirector&)            = delete;

    /** コピー代入も禁止。 */
    FPrivacyDirector& operator=(const FPrivacyDirector&) = delete;

    /** ムーブ禁止。 */
    FPrivacyDirector(FPrivacyDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FPrivacyDirector& operator=(FPrivacyDirector&&)      = delete;

    /**
     * 現在のポリシー版を設定して director を初期化する。
     *
     * @details 0 は予約値 (= 未初期化扱い) を避けるため既定 1 から始める。
     * @param current_policy_version 起動中に有効なプライバシーポリシーの版番号。保存済み版がこれより小さいと IsPolicyOutdated() が true を返す。
     */
    void Init(u32 current_policy_version = 1) noexcept;

    /**
     * 指定カテゴリの同意 bit を立てる (OR)。
     *
     * @param cat 同意するカテゴリ (複合 Analytics | Marketing も可)。
     */
    void GrantConsent(EConsentCategory cat)  noexcept;

    /**
     * 指定カテゴリの同意 bit を落とす (& ~cat)。
     *
     * @param cat 同意を取り消すカテゴリ (複合可)。
     */
    void RevokeConsent(EConsentCategory cat) noexcept;

    /** Required を除く全カテゴリを ON にする (= "Accept All")。 */
    void GrantAll()  noexcept;

    /** 全カテゴリを OFF (Required のみ) にする (= "Reject All")。 */
    void RevokeAll() noexcept;

    /**
     * 指定カテゴリの全 bit が同意済みかを返す。
     *
     * @details 部分一致は許さない (複合 cat はすべての bit が立っている必要がある)。Required (= 0) を渡した場合は仕様により常に true。
     * @param cat 確認するカテゴリ (複合可)。
     * @return cat の全 bit が同意済みなら true。
     */
    bool            HasConsent(EConsentCategory cat) const noexcept;

    /**
     * 現在同意済みのカテゴリ mask を返す。
     *
     * @return bit OR された同意 mask。
     */
    EConsentCategory GrantedMask()                   const noexcept;

    /**
     * 初回 consent ダイアログの提示が必要かを返す。
     *
     * @details GDPR では同意取得前に追跡を開始してはならないため、起動直後に呼んでダイアログ表示を強制する。Init() 前は保守的に true を返す。
     * @return まだダイアログを 1 度も提示していなければ true。
     */
    bool RequiresInitialConsent() const noexcept;

    /** ダイアログを提示しユーザーが選択を行った後に呼ぶ (以降 RequiresInitialConsent() は false)。 */
    void MarkInitialConsentShown() noexcept;

    /**
     * ロード済みのポリシー版が現在より古いかを返す。
     *
     * @details true の間は再同意ダイアログを出すこと。Init() 前は保守的に false を返す。
     * @return stored policy_version が current より小さければ true。
     */
    bool IsPolicyOutdated()      const noexcept;

    /**
     * 保存されていたポリシー版を返す。
     *
     * @return Load された同意状態の policy_version。
     */
    u32  StoredPolicyVersion()   const noexcept;

    /**
     * Init() に渡された現在のポリシー版を返す。
     *
     * @return current policy_version。
     */
    u32  CurrentPolicyVersion()  const noexcept;

    /** 初期化前の状態に戻す (mask=Required, 未提示, stored_version=0)。テスト・デバッグ用。 */
    void Reset() noexcept;

    /**
     * file_path に ConsentStatus を .acssave バイナリで保存する。
     *
     * @details 保存直前に現在の policy_version を焼き込むため、次回 Load 後の IsPolicyOutdated() 判定が成立する。下層 I/O は FSaveSlot に委譲。
     * @param file_path 保存先ファイルパス。
     * @return 成功なら空の TResult、null パス / 未 Init / I/O 失敗ならエラー。
     */
    TResult<void> SaveConsent(const wchar_t* file_path) noexcept;

    /**
     * file_path から ConsentStatus を読み出して内部状態を復元する。
     *
     * @details ファイルが存在しない場合は「初回起動」扱いで Ok() を返し (= ダイアログ強制)、復元成功時は m_bInitialConsentShown を true にする。version 不一致 (kSubMigrationNeeded) / CRC 破損 / I/O 失敗はそのまま Err で伝搬する。
     * @param file_path 読み込み元ファイルパス。
     * @return 成功または初回起動なら空の TResult、破損 / I/O 失敗ならエラー。
     */
    TResult<void> LoadConsent(const wchar_t* file_path) noexcept;

    /**
     * Save/Load が返す共通エラー subcode。
     *
     * @details
     * 下層 I/O 由来のエラー (CRC 破損 / version 不一致 / 下層 file not found 等) は
     * ESaveArchiveSubCode (gameframework/SaveArchive.h) の値で返るため、呼び出し側は
     * そちらも参照して分岐できる。
     */
    enum SubCode : u16 {
        /** Init() 未呼出で Save/Load した。 */
        kSub_NotInitialized = 1,

        /** file_path が nullptr。 */
        kSub_BadPath        = 2,
    };

private:
    /** 現在の同意状態 (永続化単位)。 */
    ConsentStatus _status{};

    /** Init() に渡された現在のポリシー版。 */
    u32           m_CurrentPolicyVersion = 0;

    /** Init() 済みかどうか。 */
    bool          m_Initialized            = false;

    /** 初回ダイアログを提示済みかどうか。 */
    bool          m_bInitialConsentShown  = false;
};

} // namespace acs::game
