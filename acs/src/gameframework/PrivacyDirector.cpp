// SPDX-License-Identifier: Apache-2.0
// GameFramework メタ層 — FPrivacyDirector 実装
//
// consent 状態は bit flag (EConsentCategory) を 1 個の u32 にまとめて持つ。
// Grant/Revoke は単純な OR / AND-NOT で、複合カテゴリ (Analytics | Marketing)
// にも対応する。Required (= 0) は「同意不要」を表すため特別扱い:
//   ・GrantConsent(Required) は何もしない (bit が無いので OR で変化なし)
//   ・RevokeConsent(Required) も同様
//   ・HasConsent(Required) は仕様により常に true
//
// Save/Load は Phase 1 では stub (NotImplemented)。Phase 2 で FSaveSlot や
// FAssetPack 経由でバイナリ永続化する予定。永続化先は ConsentStatus 構造体
// 単体を想定 (POD として完結している)。
#include "gameframework/PrivacyDirector.h"

namespace acs::game {

// ----- 内部ヘルパ -----------------------------------------------------------

// Required(=0) を除いた全カテゴリの bit 集合。GrantAll/RevokeAll で使う。
// 新カテゴリ追加時はここに OR で追記する必要がある (静的に閉じておく)。
static constexpr EConsentCategory kAllCategories =
    EConsentCategory::Analytics
    | EConsentCategory::Marketing
    | EConsentCategory::Personalization
    | EConsentCategory::ThirdPartySharing
    | EConsentCategory::Telemetry
    | EConsentCategory::CrashReports;

// EConsentCategory 同士の bit 操作ユーティリティ。
// & ~b で「b に含まれる bit を a から落とす」を表現する。
static constexpr EConsentCategory AndNot(EConsentCategory a, EConsentCategory b) noexcept {
    return static_cast<EConsentCategory>(static_cast<u32>(a) & ~static_cast<u32>(b));
}

// ----- 初期化 ---------------------------------------------------------------

void FPrivacyDirector::Init(u32 current_policy_version) noexcept {
    m_CurrentPolicyVersion = current_policy_version;
    m_Initialized            = true;
    // _status / m_InitialConsentShown は LoadConsent() で上書きされる想定。
    // 未 Load なら「初回 = 何も同意していない = Required のみ」の状態のまま。
}

// ----- 同意操作 -------------------------------------------------------------

void FPrivacyDirector::GrantConsent(EConsentCategory cat) noexcept {
    // OR で bit を立てる。複合 (Analytics | Marketing) もそのまま受理。
    // Required(=0) を渡されても OR で変化なしなので分岐不要。
    _status.granted_mask = _status.granted_mask | cat;
}

void FPrivacyDirector::RevokeConsent(EConsentCategory cat) noexcept {
    // & ~cat で bit を落とす。Required(=0) は ~0 = 全 bit になるため
    // 「Required を revoke」要求は実質 no-op。明示判定はせず数学に任せる。
    _status.granted_mask = AndNot(_status.granted_mask, cat);
}

void FPrivacyDirector::GrantAll() noexcept {
    // Required を除く全カテゴリを ON。"Accept All" ボタン相当。
    _status.granted_mask = kAllCategories;
}

void FPrivacyDirector::RevokeAll() noexcept {
    // 全カテゴリ OFF (= Required のみ)。"Reject All" ボタン相当。
    _status.granted_mask = EConsentCategory::Required;
}

// ----- 問い合わせ -----------------------------------------------------------

bool FPrivacyDirector::HasConsent(EConsentCategory cat) const noexcept {
    // Required (=0) は仕様により常に true (法的同意不要のカテゴリ)。
    // GDPR/CCPA でも「サービス提供に必要不可欠な処理」は同意なしで許される
    // ため、ローカルセーブ等を Required で分類するとこの分岐に乗る。
    if (cat == EConsentCategory::Required) return true;

    // 複合カテゴリ判定: cat の **全ての** bit が立っているかを確認する。
    // 部分一致を許すとセキュリティ上の "うっかり許可" を生む。
    const u32 mask = static_cast<u32>(_status.granted_mask);
    const u32 want = static_cast<u32>(cat);
    return (mask & want) == want;
}

EConsentCategory FPrivacyDirector::GrantedMask() const noexcept {
    return _status.granted_mask;
}

// ----- 初回ダイアログ判定 ---------------------------------------------------

bool FPrivacyDirector::RequiresInitialConsent() const noexcept {
    // Init() 前は判定不能なので「要・表示」を返す保守側にしておく
    // (= ダイアログを必ず出してから先に進ませる)。
    if (!m_Initialized) return true;
    return !m_InitialConsentShown;
}

void FPrivacyDirector::MarkInitialConsentShown() noexcept {
    m_InitialConsentShown = true;
}

// ----- ポリシー版管理 -------------------------------------------------------

bool FPrivacyDirector::IsPolicyOutdated() const noexcept {
    // 保存済み policy_version が「現在」より古ければ再同意が必要。
    // Init() 前は current が 0 のままなので、未初期化なら常に false (= 古くない)。
    // この保守側により Init() 忘れで誤ったダイアログを出す事故を防ぐ。
    if (!m_Initialized) return false;
    return _status.policy_version < m_CurrentPolicyVersion;
}

u32 FPrivacyDirector::StoredPolicyVersion() const noexcept {
    return _status.policy_version;
}

u32 FPrivacyDirector::CurrentPolicyVersion() const noexcept {
    return m_CurrentPolicyVersion;
}

// ----- デバッグ -------------------------------------------------------------

void FPrivacyDirector::Reset() noexcept {
    // テスト用。本番フローでは呼ばない。
    _status                 = ConsentStatus{};
    m_CurrentPolicyVersion = 0;
    m_Initialized            = false;
    m_InitialConsentShown  = false;
}

// ----- 永続化 (Phase 1 stub) ------------------------------------------------

TResult<void> FPrivacyDirector::SaveConsent(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_BadPath,
                       "FPrivacyDirector::SaveConsent received null path");
    }
    if (!m_Initialized) {
        return ACS_ERR(IO, kSub_NotInitialized,
                       "FPrivacyDirector::SaveConsent called before Init()");
    }
    // Phase 2 の擬似コード:
    //   1. FSaveSlot<ConsentStatus> slot; slot.Init(file_path);
    //   2. slot.Save(_status);
    //   3. エラーは TResult<void> でそのまま伝搬。
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FPrivacyDirector::SaveConsent is not yet implemented (Phase 1 stub)");
}

TResult<void> FPrivacyDirector::LoadConsent(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_BadPath,
                       "FPrivacyDirector::LoadConsent received null path");
    }
    if (!m_Initialized) {
        return ACS_ERR(IO, kSub_NotInitialized,
                       "FPrivacyDirector::LoadConsent called before Init()");
    }
    // Phase 2 の擬似コード:
    //   1. FSaveSlot<ConsentStatus> slot; slot.Init(file_path);
    //   2. if (!slot.Exists()) return Ok();   // 初回起動扱い (ダイアログ強制)
    //   3. auto r = slot.Load();
    //   4. if (r) {
    //        _status = r.Value();
    //        m_InitialConsentShown = true;   // 過去に同意済み → ダイアログ不要
    //      }
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FPrivacyDirector::LoadConsent is not yet implemented (Phase 1 stub)");
}

} // namespace acs::game
