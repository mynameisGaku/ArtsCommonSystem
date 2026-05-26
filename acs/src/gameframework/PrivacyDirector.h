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
//   ・**Save/Load は stub**: 永続化は AssetPack/FSaveSlot 接続後に Phase 2 で
//     実装する。現段階では API 形状だけ確定し、ACS_ERR(IO, kSub_NotImplemented)
//     を返す stub にしておく (FSaveSlot 等と同じ規約)。
//   ・**非コピー・非ムーブ**: アプリ全体で 1 個運用される director なので、
//     誤って値渡しされて consent 状態が分裂しないよう移動コンストラクタも禁止。
//   ・**全 noexcept**: ACS 規約に従い例外なし。TResult<void, FErrorCode> で
//     エラーを伝搬する。
//
// 範囲外 (Phase 2+ で):
//   ・Save/Load の実 I/O 接続 (FSaveSlot/AssetPack 経由でバイナリ永続化)
//   ・consent 取得 UI そのもの (ゲーム側 / Editor 側で別途実装)
//   ・地域判定 (GDPR 地域だけダイアログを強制する 等の policy)
//   ・consent 変更時の subscribe コールバック (今は pull 型のみ)
//   ・Data Subject Access Request (DSAR): ユーザーが「自分のデータを見せろ」
//     と要求した場合のエクスポート (Pillar V Backend Services 側で扱う想定)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// EConsentCategory — bit flag で表す同意カテゴリ
// -----------------------------------------------------------------------------
// 各カテゴリは独立に on/off できる必要がある (GDPR granular consent)。
// Required (= 0) は「同意不要 = 常に許可」を表す特異値で、ゲームの基本動作に
// 必要なサービス (ローカルセーブ等) を分類するためのマーカ。HasConsent()
// に Required を渡した場合は常に true。
// =============================================================================
enum class EConsentCategory : u32 {
    Required          = 0,         // 法的同意不要 (常に許可される基本機能)
    Analytics         = 1u << 0,   // ゲームプレイ統計 / 行動ログ
    Marketing         = 1u << 1,   // 広告 / 販促 / プッシュ通知
    Personalization   = 1u << 2,   // 個人向けレコメンド / カスタマイズ
    ThirdPartySharing = 1u << 3,   // SDK / 外部パートナーへのデータ共有
    Telemetry         = 1u << 4,   // パフォーマンス計測 / 匿名診断
    CrashReports      = 1u << 5,   // クラッシュ収集 (個別 opt-out 用)
};

constexpr EConsentCategory operator|(EConsentCategory a, EConsentCategory b) noexcept {
    return static_cast<EConsentCategory>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr EConsentCategory operator&(EConsentCategory a, EConsentCategory b) noexcept {
    return static_cast<EConsentCategory>(static_cast<u32>(a) & static_cast<u32>(b));
}

// =============================================================================
// FConsentStatus — 永続化単位の POD
// -----------------------------------------------------------------------------
// SaveConsent / LoadConsent で書き出される最小構造体。trivially-copyable な
// POD として扱えるよう member は全て値型のみ。policy_version は再同意
// 判定の主軸で、consent_timestamp は監査ログ向けに「いつ同意したか」を
// epoch ms 等で保持する想定 (Phase 2 で TimeSource を確定したら代入)。
// =============================================================================
struct FConsentStatus {
    EConsentCategory granted_mask      = EConsentCategory::Required;  // bit OR された同意 mask
    u64             consent_timestamp = 0;                          // 最後に同意した時刻 (epoch ms 等)
    u32             policy_version    = 0;                          // 同意時のポリシー版
};

// =============================================================================
// FPrivacyDirector — consent 管理ハブ
// -----------------------------------------------------------------------------
// アプリ全体で 1 個運用される想定。複数同時運用しないため非コピー・非ムーブ。
// =============================================================================
class FPrivacyDirector {
public:
    FPrivacyDirector()  noexcept = default;
    ~FPrivacyDirector() noexcept = default;

    FPrivacyDirector(const FPrivacyDirector&)            = delete;
    FPrivacyDirector& operator=(const FPrivacyDirector&) = delete;
    FPrivacyDirector(FPrivacyDirector&&)                 = delete;
    FPrivacyDirector& operator=(FPrivacyDirector&&)      = delete;

    // ----- 初期化 -----
    // current_policy_version: 起動中に有効なプライバシーポリシーの版番号。
    //   保存済み版がこれより小さければ IsPolicyOutdated() が true を返す。
    // 0 は予約値 (= 未初期化扱い) を避けるため既定 1 から始める。
    void Init(u32 current_policy_version = 1) noexcept;

    // ----- 同意操作 (bit 単位) -----
    // GrantConsent: 指定カテゴリの bit を立てる (OR)。
    // RevokeConsent: 指定カテゴリの bit を落とす (& ~cat)。
    // どちらも複合 (`Analytics | Marketing`) を渡せる。
    void GrantConsent(EConsentCategory cat)  noexcept;
    void RevokeConsent(EConsentCategory cat) noexcept;

    // ----- 一括操作 -----
    // GrantAll: Required を除く全カテゴリを ON にする (= "Accept All")。
    // RevokeAll: 全カテゴリを OFF (Required のみ) にする (= "Reject All")。
    void GrantAll()  noexcept;
    void RevokeAll() noexcept;

    // ----- 問い合わせ -----
    // HasConsent: cat の **全ての** bit が立っているかを確認 (複合 cat 可)。
    //   Required (= 0) を渡した場合は仕様により常に true。
    bool            HasConsent(EConsentCategory cat) const noexcept;
    EConsentCategory GrantedMask()                   const noexcept;

    // ----- 初回ダイアログ判定 -----
    // RequiresInitialConsent: まだ consent ダイアログを 1 度も提示していないなら true。
    //   GDPR では同意取得前に追跡を開始してはならないので、起動直後に呼んで
    //   ダイアログ表示を強制する用途。
    bool RequiresInitialConsent() const noexcept;

    // MarkInitialConsentShown: ダイアログを提示してユーザーが何らかの選択を
    //   行った後に呼ぶ。以降 RequiresInitialConsent() は false を返す。
    void MarkInitialConsentShown() noexcept;

    // ----- ポリシー版管理 -----
    // IsPolicyOutdated: ロード済み policy_version が現在より古ければ true。
    //   true の間は再同意ダイアログを出すこと。
    bool IsPolicyOutdated()      const noexcept;
    u32  StoredPolicyVersion()   const noexcept;
    u32  CurrentPolicyVersion()  const noexcept;

    // ----- デバッグ -----
    // Reset: 初期化前の状態に戻す (= mask=Required, 未提示, stored_version=0)。
    //   テストとデバッグ用。本番フローでは呼ばない。
    void Reset() noexcept;

    // ----- 永続化 (Phase 1: stub) -----
    // SaveConsent: file_path に FConsentStatus をバイナリ保存する。
    //   Phase 1 では ACS_ERR(IO, kSub_NotImplemented) を返す stub。
    // LoadConsent: file_path から FConsentStatus を読み出して内部状態を復元する。
    //   Phase 1 では ACS_ERR(IO, kSub_NotImplemented) を返す stub。
    TResult<void> SaveConsent(const wchar_t* file_path) noexcept;
    TResult<void> LoadConsent(const wchar_t* file_path) noexcept;

    // 共通エラー subcode (TResult<...,FErrorCode> の subcode に入る)。
    enum SubCode : u16 {
        kSub_NotInitialized = 1,  // Init() 未呼出で Save/Load した
        kSub_BadPath        = 2,  // file_path が nullptr
        kSub_NotImplemented = 99, // Phase 1 stub
    };

private:
    FConsentStatus _status{};                 // 現在の同意状態
    u32           _current_policy_version = 0; // Init() に渡された版
    bool          _initialized            = false; // Init() 済みか
    bool          _initial_consent_shown  = false; // 初回ダイアログを提示したか
};

} // namespace acs::game
