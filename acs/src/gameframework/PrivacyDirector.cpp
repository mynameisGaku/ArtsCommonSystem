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
// Save/Load は FSaveSlot<ConsentStatus> 経由でローカルにバイナリ永続化する。
// GDPR/CCPA の同意情報は端末ローカルに保持する性質のものなので、プラット
// フォーム backend は不要 (クラウド同期は将来の別案件)。永続化単位は
// ConsentStatus 構造体単体 (POD として完結している)。
#include "gameframework/PrivacyDirector.h"

#include "gameframework/SaveSlot.h"   // FSaveSlot<T> (FSaveArchive 経由の永続化)

namespace acs::game {

// ----- 永続化フォーマット定数 ----------------------------------------------

// consent ファイルの schema version。policy_version (= ポリシー文言の版) とは
// 別物で、こちらは ConsentStatus の **バイナリレイアウト** に対する版番号。
// レイアウトを変えたら +1 し、旧ファイル読込時に kSubMigrationNeeded で検知する。
static constexpr u32 kConsentSchemaVersion = 1u;

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
    // _status / m_bInitialConsentShown は LoadConsent() で上書きされる想定。
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
    return !m_bInitialConsentShown;
}

void FPrivacyDirector::MarkInitialConsentShown() noexcept {
    m_bInitialConsentShown = true;
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
    m_bInitialConsentShown  = false;
}

// ----- 永続化 ---------------------------------------------------------------

TResult<void> FPrivacyDirector::SaveConsent(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_BadPath,
                       "FPrivacyDirector::SaveConsent received null path");
    }
    if (!m_Initialized) {
        return ACS_ERR(IO, kSub_NotInitialized,
                       "FPrivacyDirector::SaveConsent called before Init()");
    }

    // 保存直前に「現在のポリシー版」を同意状態へ焼き込む。これにより次回起動の
    // LoadConsent + IsPolicyOutdated() 判定が成立する (= 古いポリシーで同意した
    // ことが stored < current として検出できる)。
    _status.policy_version = m_CurrentPolicyVersion;

    // ConsentStatus は POD (enum / u64 / u32 のみ) なので FSaveSlot にそのまま
    // 委譲できる。Save 内部で FSaveArchive が 24B header + payload + CRC32 を
    // little-endian で書き出す。
    FSaveSlot<ConsentStatus> slot;
    slot.Init(file_path);
    return slot.Save(_status, kConsentSchemaVersion);
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

    FSaveSlot<ConsentStatus> slot;
    slot.Init(file_path);

    // ファイルが無いのは「初回起動」= エラーではない。_status は Required のみの
    // ままにして Ok() を返し、RequiresInitialConsent() に同意ダイアログを強制
    // させる (m_bInitialConsentShown は触らない)。
    if (!slot.Exists()) {
        return Ok();
    }

    auto r = slot.Load(kConsentSchemaVersion);
    if (r.IsErr()) {
        // version 不一致 (kSubMigrationNeeded) / CRC 破損 / I/O 失敗はそのまま
        // 伝搬する。呼び出し側は「壊れた consent → 再同意」を選べるが、ここで
        // 勝手に Ok 扱いにはしない (誤って追跡を開始する事故を避ける)。
        return r.Error();
    }

    // 復元成功: 過去に同意操作が行われた証拠なので、初回ダイアログは不要にする。
    // (全カテゴリ reject = mask が Required のみでも「ダイアログは提示済み」)。
    _status                = r.Value();
    m_bInitialConsentShown = true;
    return Ok();
}

} // namespace acs::game
