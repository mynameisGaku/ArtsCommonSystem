// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FSteamworksBridge 実装 (Stub のみ)
//
// 本 .cpp では `ISteamworksBridge` の I/F 自体は提供せず、Steamworks SDK 未統合
// ビルドでも常に使える `SteamworksBridgeStub` のみを実装する。実 SDK を使う
// `GoldenSteamworksBridge` は Steamworks SDK ヘッダ / ライブラリ依存を含むため、
// 別モジュール (`acs_steamworks` 等) で独立に実装し、本ファイルには持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。`Init()` は単に `m_Initialized = true` を立てるのみ。
//   ・Achievement / Leaderboard 系は `ACS_ERR(Generic, kSubSteamworksNotImplemented,
//     ...)` を返し、上位ロジックでサイレントに無視するか or ログ出力するかを
//     呼び出し側に委ねる。
//   ・`GetStub()` は Meyer's singleton。スレッド初回構築は C++11 以降の規格で
//     保証されているため、追加同期は不要。
//   ・文字列リテラルは static storage duration なので、PlayerIdentity に
//     そのまま入れても寿命は永続。

#include "gameframework/SteamworksBridge.h"

#include "foundation/Error.h"
#include "threading/Atomic.h"

namespace acs::game {

/** Stub を初期化済みにマークする (実 SDK と違い常に成功する)。 */
TResult<void> SteamworksBridgeStub::Init() noexcept {
    // 多重 Init は明示的に許容する。実 SDK の SteamAPI_Init() は失敗時 false を
    // 返すが、Stub はテスト容易性のため常に成功。
    m_Initialized = true;
    return Ok();
}

/** 初期化フラグを下ろす (Init() 前に呼んでも安全な no-op)。 */
void SteamworksBridgeStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。
    m_Initialized = false;
}

/** 固定ダミーのローカルプレイヤー情報を返す (未初期化時は全フィールド空)。 */
PlayerIdentity SteamworksBridgeStub::GetLocalPlayer() const noexcept {
    PlayerIdentity id{};
    if (!m_Initialized) {
        // 未初期化時は全フィールド空のまま返す (Bridge は throw しない方針)。
        return id;
    }
    // Stub の固定ダミー値。`platform_id` / `display_name` は string literal なので
    // プロセス終了まで生存可能。
    id.platform_id   = "stub_player";
    id.display_name  = "Player";
    id.session_token = 0;  // Stub にはセッション概念がない
    return id;
}

/** 実績解除は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::UnlockAchievement(const char* ach_id) noexcept {
    (void)ach_id;  // 未使用引数 (Stub なので no-op)
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::UnlockAchievement called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: UnlockAchievement is not implemented (link real SDK)");
}

/** リーダーボードへの送信は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::SetLeaderboardScore(const char* board_id, i64 score) noexcept {
    (void)board_id;
    (void)score;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetLeaderboardScore called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetLeaderboardScore is not implemented (link real SDK)");
}

/** リーダーボードスコアの取得は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<i64> SteamworksBridgeStub::GetLeaderboardScore(const char* board_id) noexcept {
    (void)board_id;
    if (!m_Initialized) {
        return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetLeaderboardScore called before Init()"));
    }
    return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetLeaderboardScore is not implemented (link real SDK)"));
}

/** Stub は callback pump を持たないので何もしない。 */
void SteamworksBridgeStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は callback pump を持たないので何もしない
}

/** 整数 stat の設定は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::SetStat(const char* stat_name, i64 value) noexcept {
    (void)stat_name; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetStat called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetStat is not implemented (link real SDK)");
}

/** 整数 stat の取得は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<i64> SteamworksBridgeStub::GetStat(const char* stat_name) noexcept {
    (void)stat_name;
    if (!m_Initialized) {
        return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetStat called before Init()"));
    }
    return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetStat is not implemented (link real SDK)"));
}

/** 浮動小数 stat の設定は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::SetFloatStat(const char* stat_name, f32 value) noexcept {
    (void)stat_name; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetFloatStat called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetFloatStat is not implemented (link real SDK)");
}

/** 浮動小数 stat の取得は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<f32> SteamworksBridgeStub::GetFloatStat(const char* stat_name) noexcept {
    (void)stat_name;
    if (!m_Initialized) {
        return TResult<f32>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetFloatStat called before Init()"));
    }
    return TResult<f32>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetFloatStat is not implemented (link real SDK)"));
}

/** Stub は何も所有していないので常に false を返す。 */
bool SteamworksBridgeStub::IsDlcOwned(u32 app_id) const noexcept {
    (void)app_id;
    return false;  // Stub: 何も所有していない
}

/** RichPresence の設定は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::SetRichPresence(const char* key, const char* value) noexcept {
    (void)key; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetRichPresence called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetRichPresence is not implemented (link real SDK)");
}

/** Stub はフレンドリストを持たないので常に 0 を返す。 */
u32 SteamworksBridgeStub::GetFriendCount() const noexcept {
    return 0;  // Stub: フレンドリスト無し
}

/** Stub はフレンドを持たないので常に空の PlayerIdentity を返す。 */
PlayerIdentity SteamworksBridgeStub::GetFriendByIndex(u32 index) const noexcept {
    (void)index;
    return PlayerIdentity{};  // Stub: 空
}

/** Cloud への書き込みは未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::CloudWriteFile(const char* path, const void* data, u32 size) noexcept {
    (void)path; (void)data; (void)size;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudWriteFile before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudWriteFile");
}

/** Cloud からの読み出しは未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<u32> SteamworksBridgeStub::CloudReadFile(const char* path, void* out_buf, u32 buf_size) noexcept {
    (void)path; (void)out_buf; (void)buf_size;
    if (!m_Initialized) {
        return TResult<u32>(ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudReadFile before Init()"));
    }
    return TResult<u32>(ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudReadFile"));
}

/** Stub は Cloud ファイルを持たないので常に false を返す。 */
bool SteamworksBridgeStub::CloudFileExists(const char* path) const noexcept {
    (void)path;
    return false;
}

/** Cloud からの削除は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::CloudDeleteFile(const char* path) noexcept {
    (void)path;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudDeleteFile before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudDeleteFile");
}

/** Stub は容量情報を持たないので空き・総容量とも 0 を返す。 */
void SteamworksBridgeStub::CloudGetQuota(u64& out_available_bytes, u64& out_total_bytes) const noexcept {
    out_available_bytes = 0;
    out_total_bytes     = 0;
}

/** Stub はサブスクライブ済み Workshop アイテムを持たないので常に 0 を返す。 */
u32 SteamworksBridgeStub::WorkshopGetSubscribedCount() const noexcept {
    return 0;
}

/** Stub は Workshop アイテムを持たないので item_id=0 / install_path=nullptr を返す。 */
void SteamworksBridgeStub::WorkshopGetSubscribedItem(u32 index, u64& out_item_id,
                                                     const char*& out_install_path) const noexcept {
    (void)index;
    out_item_id      = 0;
    out_install_path = nullptr;
}

/** ボイス録音開始は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::VoiceStartRecording() noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "VoiceStartRecording before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: VoiceStartRecording");
}

/** ボイス録音停止は副作用が無いので常に成功を返す。 */
TResult<void> SteamworksBridgeStub::VoiceStopRecording() noexcept {
    return Ok();  // 副作用無し
}

/** Stub は音声データを持たないので取得 byte 数 0 で成功を返す。 */
TResult<u32> SteamworksBridgeStub::VoiceGetCompressed(void* out_buf, u32 buf_size) noexcept {
    (void)out_buf; (void)buf_size;
    return TResult<u32>(OkInit, 0u);  // データなし
}

/** Steam Input の初期化は未実装。Init() 前なら NotInitialized、それ以外は NotImplemented を返す。 */
TResult<void> SteamworksBridgeStub::InputInit() noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "InputInit before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: InputInit");
}

/** Stub は接続コントローラを持たないので常に 0 を返す。 */
u32 SteamworksBridgeStub::InputGetControllerCount() const noexcept {
    return 0;
}

/** プロセス共有の Stub singleton を返す (関数スコープ static で thread-safe 初期化)。 */
SteamworksBridgeStub& SteamworksBridgeStub::GetStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static SteamworksBridgeStub m_Instance;
    return m_Instance;
}

namespace {
/** 登録済みの既定 Bridge provider (nullptr なら Stub を使う)。 */
TAtomic<SteamworksBridgeProvider> g_SteamworksBridgeProvider{nullptr};
}

/** 既定 Bridge provider を差し替える (nullptr で Stub に戻す。後勝ち)。 */
void SetSteamworksBridgeProvider(SteamworksBridgeProvider Provider) noexcept
{
    g_SteamworksBridgeProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実 Bridge を、未登録なら Stub singleton を返す。 */
ISteamworksBridge& GetDefaultSteamworksBridge() noexcept
{
    const SteamworksBridgeProvider Provider = g_SteamworksBridgeProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : SteamworksBridgeStub::GetStub();
}

} // namespace acs::game
