// SPDX-License-Identifier: Apache-2.0
// Steamworks SDK の real backend 実装。
// `acs::game::ISteamworksBridge` を Override し、SteamAPI_Init() 等の本物の
// SDK 関数を呼ぶ。ビルド有効化は CMake の ACS_BUILD_STEAMWORKS=ON。
//
// 利用方法:
//   #include "steamworks/SteamworksBridgeImpl.h"
//   acs::steamworks::FSteamworksBridgeImpl impl;
//   if (impl.Init().IsErr()) {
//       // Steam クライアントが起動していない / AppID 不一致 等
//       fallback_to_stub();
//   }
//   m_Social = &impl;
//
// 設計:
//   ・SteamAPI_Init() を Init() 内で呼び、Steam クライアント連携を確立。
//   ・GetLocalPlayer() は ISteamFriends::GetPersonaName() + SteamID64 を返す。
//   ・UnlockAchievement(name) は ISteamUserStats::SetAchievement() + StoreStats() を即時呼び出し。
//   ・SetLeaderboardScore は async (Find -> Upload の 2 段呼出)。同期 wait は
//     Tick() 経由でコールバックポンプを進めて完了を待つ (= 数フレーム遅延)。
//   ・GetLeaderboardScore も同様に async (Find -> Download)。
//   ・Tick(dt) で SteamAPI_RunCallbacks() を毎フレーム呼ぶ (必須)。
//
// 注意:
//   ・Steam クライアントが起動していないと SteamAPI_Init() は false を返す。
//   ・dev では steam_appid.txt = 480 (Spacewar) を実行 dir に置く。
//   ・Release では Steam Direct で取得した本番 AppID を steam_appid.txt に。
#pragma once

#include "foundation/Result.h"
#include "gameframework/SteamworksBridge.h"

namespace acs::steamworks {

/** SteamAPI_Init 失敗を表す subcode (gameframework の SubSteamworks* の続き)。 */
inline constexpr u16 kSubSteamworksInitFailed       = 1003;

/** FindLeaderboard 失敗を表す subcode。 */
inline constexpr u16 kSubSteamworksFindBoardFailed  = 1004;

/** UploadLeaderboardScore 失敗を表す subcode。 */
inline constexpr u16 kSubSteamworksUploadFailed     = 1005;

/** DownloadLeaderboardEntries 失敗を表す subcode。 */
inline constexpr u16 kSubSteamworksDownloadFailed   = 1006;

/** async API のタイムアウト (結果未着) を表す subcode。 */
inline constexpr u16 kSubSteamworksTimeout          = 1007;

/**
 * Steamworks SDK を呼ぶ ISteamworksBridge の実 backend 実装。
 *
 * @details
 * SteamAPI_Init() 等の本物の SDK 関数を呼んで Steam クライアント連携を確立する。
 * SDK ヘッダ (steam_api.h) を public ヘッダから漏らさないため Pimpl で実装を隠し、
 * leaderboard 系は async (Find -> Upload/Download) の 2 段で進めて Tick() の
 * コールバックポンプで完了を回収する。CMake の ACS_BUILD_STEAMWORKS=ON でビルドされる。
 */
class FSteamworksBridgeImpl final : public acs::game::ISteamworksBridge {
public:
    /** Pimpl 状態を確保して構築する (Steam 連携は Init で確立)。 */
    FSteamworksBridgeImpl() noexcept;

    /** 必要なら Shutdown を呼び、Pimpl 状態を解放する。 */
    ~FSteamworksBridgeImpl() noexcept override;

    /** コピー禁止 (Pimpl 状態を単独所有するため)。 */
    FSteamworksBridgeImpl(const FSteamworksBridgeImpl&)            = delete;

    /** コピー代入も禁止。 */
    FSteamworksBridgeImpl& operator=(const FSteamworksBridgeImpl&) = delete;

    /** ムーブ禁止。 */
    FSteamworksBridgeImpl(FSteamworksBridgeImpl&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FSteamworksBridgeImpl& operator=(FSteamworksBridgeImpl&&)      = delete;

    /**
     * SteamAPI_Init() を呼んで Steam クライアント連携を確立する。
     *
     * @details
     * 成功時は persona name と SteamID を初回 cache する (Tick(0) を 1 回呼ぶ)。
     * Steam クライアント未起動 / steam_appid.txt 不在・AppID 不一致 / SDK 不整合の
     * いずれでも SteamAPI_Init() が false を返し、エラーになる。多重呼び出しは安全。
     * @return 成功なら空の TResult、SteamAPI_Init 失敗ならエラー。
     */
    acs::TResult<void>                Init()                          noexcept override;

    /** SteamAPI_Shutdown() を呼んで連携を終了する (未初期化なら何もしない)。 */
    void                              Shutdown()                      noexcept override;

    /**
     * Steam 連携が確立済みかを返す。
     *
     * @return Init 成功後で未 Shutdown なら true。
     */
    bool                              IsInitialized()           const noexcept override;

    /**
     * ローカルプレイヤーの識別情報を返す。
     *
     * @details display_name は ISteamFriends::GetPersonaName()、platform_id は
     * SteamID64 文字列。文字列の実体は本クラスが保持し Tick で更新される。
     * @return ローカルプレイヤーの PlayerIdentity (未初期化なら空)。
     */
    acs::game::PlayerIdentity         GetLocalPlayer()          const noexcept override;

    /**
     * 実績を解除する (SetAchievement + StoreStats を即時実行)。
     *
     * @param ach_id 解除する実績の API name。
     * @return 成功なら空の TResult、未初期化・引数不正・SDK 取得失敗ならエラー。
     */
    acs::TResult<void>                UnlockAchievement(const char* ach_id)               noexcept override;

    /**
     * リーダーボードへスコアを送信する (async)。
     *
     * @details
     * FindLeaderboard を kick off し、完了コールバックで UploadLeaderboardScore
     * (KeepBest) を起動する。pending op として固定プールに積み、Tick で進める。
     * @param board_id 対象リーダーボードの API name。
     * @param score 送信するスコア (SDK では int32 へ clamp される)。
     * @return 成功なら空の TResult、未初期化・引数不正・プール満杯ならエラー。
     */
    acs::TResult<void>                SetLeaderboardScore(const char* board_id, acs::i64 score) noexcept override;

    /**
     * リーダーボードのスコアを取得する (async)。
     *
     * @details
     * 同じ board_id の download op が完了済みならそのスコアを返す。未完了/未発行なら
     * FindLeaderboard -> DownloadLeaderboardEntries を kick off し、結果が無いため
     * Timeout エラーを返す。Tick で進めてから再呼び出しすると相関の取れた結果を返す。
     * @param board_id 対象リーダーボードの API name。
     * @return 取得済みスコア、または pending/失敗を表すエラー。
     */
    acs::TResult<acs::i64>            GetLeaderboardScore(const char* board_id)           noexcept override;

    /**
     * 毎フレーム呼んでコールバックを進め、persona/SteamID と pending op を更新する。
     *
     * @details
     * SteamAPI_RunCallbacks() を呼び、persona name / SteamID を refresh、async
     * leaderboard op プールを compact する (成功 download は board ごとに 1 件保持)。
     * @param dt 前フレームからの経過秒 (本実装では未使用)。
     */
    void                              Tick(acs::f32 dt)               noexcept override;

    /**
     * 整数 stat を設定する (SetStat + StoreStats、int32 へ clamp)。
     *
     * @param stat_name 対象 stat の API name。
     * @param value 設定する値 (int32 範囲へ clamp される)。
     * @return 成功なら空の TResult、未初期化・引数不正・SDK 取得失敗ならエラー。
     */
    acs::TResult<void>                SetStat(const char* stat_name, acs::i64 value)      noexcept override;

    /**
     * 整数 stat を取得する。
     *
     * @param stat_name 対象 stat の API name。
     * @return 取得した値、または未初期化・引数不正・SDK 取得失敗を表すエラー。
     */
    acs::TResult<acs::i64>            GetStat(const char* stat_name)                      noexcept override;

    /**
     * 浮動小数 stat を設定する (SetStat の float overload + StoreStats)。
     *
     * @param stat_name 対象 stat の API name。
     * @param value 設定する値。
     * @return 成功なら空の TResult、未初期化・引数不正・SDK 取得失敗ならエラー。
     */
    acs::TResult<void>                SetFloatStat(const char* stat_name, acs::f32 value) noexcept override;

    /**
     * 浮動小数 stat を取得する。
     *
     * @param stat_name 対象 stat の API name。
     * @return 取得した値、または未初期化・引数不正・SDK 取得失敗を表すエラー。
     */
    acs::TResult<acs::f32>            GetFloatStat(const char* stat_name)                 noexcept override;

    /**
     * 指定 AppID の DLC を所有・インストール済みかを返す。
     *
     * @param app_id 確認する DLC の AppID。
     * @return インストール済みなら true (未初期化・SDK 取得失敗なら false)。
     */
    bool                              IsDlcOwned(acs::u32 app_id)                   const noexcept override;

    /**
     * リッチプレゼンスのキーと値を設定する。
     *
     * @param key リッチプレゼンスのキー。
     * @param value 設定する値。
     * @return 成功なら空の TResult、未初期化・引数不正・SDK 失敗ならエラー。
     */
    acs::TResult<void>                SetRichPresence(const char* key, const char* value) noexcept override;

    /**
     * フレンド数を返す (即時フレンドのみ)。
     *
     * @return フレンド数 (未初期化・SDK 取得失敗なら 0)。
     */
    acs::u32                          GetFriendCount()                              const noexcept override;

    /**
     * index 番目のフレンドの識別情報を返す。
     *
     * @details 文字列実体は thread_local バッファに保持され、次回呼び出しで上書きされる。
     * @param index フレンドのインデックス (即時フレンド列)。
     * @return 対象フレンドの PlayerIdentity (未初期化・範囲外なら空)。
     */
    acs::game::PlayerIdentity         GetFriendByIndex(acs::u32 index)              const noexcept override;

    /**
     * Steam Cloud へファイルを書き込む。
     *
     * @param path 書き込み先のクラウドファイルパス。
     * @param data 書き込むデータの先頭。
     * @param size 書き込むバイト数。
     * @return 成功なら空の TResult、未初期化・SDK 取得失敗・書き込み失敗ならエラー。
     */
    acs::TResult<void>                CloudWriteFile(const char* path, const void* data, acs::u32 size) noexcept override;

    /**
     * Steam Cloud からファイルを読み込む。
     *
     * @param path 読み込むクラウドファイルパス。
     * @param out_buf 読み込み先のバッファ。
     * @param buf_size 読み込み先バッファのバイト数。
     * @return 実際に読み込んだバイト数、または未初期化・SDK 取得失敗を表すエラー。
     */
    acs::TResult<acs::u32>            CloudReadFile(const char* path, void* out_buf, acs::u32 buf_size) noexcept override;

    /**
     * Steam Cloud に指定ファイルが存在するかを返す。
     *
     * @param path 確認するクラウドファイルパス。
     * @return 存在すれば true (未初期化・SDK 取得失敗なら false)。
     */
    bool                              CloudFileExists(const char* path)             const noexcept override;

    /**
     * Steam Cloud のファイルを削除する。
     *
     * @details 存在しないパスも成功扱い (SDK の FileDelete に従う)。
     * @param path 削除するクラウドファイルパス。
     * @return 成功なら空の TResult、未初期化・SDK 取得失敗ならエラー。
     */
    acs::TResult<void>                CloudDeleteFile(const char* path)             noexcept override;

    /**
     * Steam Cloud のクォータ (空き / 総量) を返す。
     *
     * @param out_available_bytes 空きバイト数を受け取る出力 (失敗時 0)。
     * @param out_total_bytes 総バイト数を受け取る出力 (失敗時 0)。
     */
    void                              CloudGetQuota(acs::u64& out_available_bytes, acs::u64& out_total_bytes) const noexcept override;

    /**
     * 購読中の Workshop アイテム数を返す。
     *
     * @return 購読アイテム数 (未初期化・SDK 取得失敗なら 0)。
     */
    acs::u32                          WorkshopGetSubscribedCount()                  const noexcept override;

    /**
     * index 番目の購読 Workshop アイテムの ID とインストールパスを返す。
     *
     * @details インストールパスは thread_local バッファに保持され次回呼び出しで上書きされる。
     * @param index 購読アイテムのインデックス。
     * @param out_item_id PublishedFileId を受け取る出力 (失敗時 0)。
     * @param out_install_path インストールパスを受け取る出力 (失敗時 nullptr)。
     */
    void                              WorkshopGetSubscribedItem(acs::u32 index, acs::u64& out_item_id, const char*& out_install_path) const noexcept override;

    /**
     * ボイス録音を開始する。
     *
     * @return 成功なら空の TResult、未初期化・SDK 取得失敗ならエラー。
     */
    acs::TResult<void>                VoiceStartRecording()                         noexcept override;

    /**
     * ボイス録音を停止する (未初期化なら何もしない)。
     *
     * @return 常に成功の空 TResult。
     */
    acs::TResult<void>                VoiceStopRecording()                          noexcept override;

    /**
     * 録音済みボイスを圧縮形式で取り出す。
     *
     * @param out_buf 圧縮ボイスを書き込むバッファ。
     * @param buf_size 出力バッファのバイト数。
     * @return 書き込んだバイト数 (データ無しは 0)、または SDK 取得失敗・取得エラー。
     */
    acs::TResult<acs::u32>            VoiceGetCompressed(void* out_buf, acs::u32 buf_size) noexcept override;

    /**
     * Steam Input を初期化する。
     *
     * @return 成功なら空の TResult、未初期化・SDK 取得失敗・Init 失敗ならエラー。
     */
    acs::TResult<void>                InputInit()                                   noexcept override;

    /**
     * 接続中のコントローラ数を返す。
     *
     * @return 接続コントローラ数 (未初期化・SDK 取得失敗なら 0)。
     */
    acs::u32                          InputGetControllerCount()                     const noexcept override;

private:
    /** Pimpl 本体 (SteamworksBridgeImpl.cpp で定義、SDK ヘッダを隠す)。 */
    struct Impl;

    /** Pimpl 状態へのポインタ (コンストラクタで確保、デストラクタで解放)。 */
    Impl* m_Impl = nullptr;

    /** Steam 連携が確立済みかを示すフラグ。 */
    bool  m_bInitialized = false;
};

/**
 * プロセス共有 singleton の実 Bridge を返す。
 *
 * @details
 * 初回アクセス時に Init() を 1 回走らせる。Steam クライアント未起動等で失敗しても
 * 参照は返すため、呼び出し側は IsInitialized 等で利用可否を判断する。
 * @return プロセス共有の ISteamworksBridge 参照。
 */
acs::game::ISteamworksBridge& GetDefaultSteamworksBridge() noexcept;

/**
 * 既定 Bridge provider を gameframework に登録する。
 *
 * @details
 * gameframework は ACS::Steamworks に依存できない (循環依存) ため、backend 側から
 * acs::game::SetSteamworksBridgeProvider() を呼んで結線する。起動時に一度だけ呼ぶと、
 * 以降は backend 非依存に acs::game::GetDefaultSteamworksBridge() で実 Bridge を取得できる。
 */
void InstallSteamworksAsDefault() noexcept;

} // namespace acs::steamworks
