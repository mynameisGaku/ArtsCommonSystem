// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FSteamworksBridge (Achievements / Leaderboards / PlayerIdentity)
//
// Steam / EOS / PS / Xbox / Switch といったプラットフォーム固有 SDK へ橋渡しする
// 「シーム (seam)」インターフェース。ゲーム側コードは ISteamworksBridge 経由でのみ
// 実績解除 / リーダーボード / ローカルプレイヤー情報を取得し、実 SDK (Steamworks
// SDK 等) との結合はビルド時の選択で差し替える。
//
// 使い方:
//   class FGame {
//       acs::game::ISteamworksBridge* m_Social = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは GoldenSteamworksBridge を DI、開発ビルドでは Stub。
//           m_Social = &acs::game::SteamworksBridgeStub::GetStub();
//           (void)m_Social->Init();
//       }
//       void OnTick(f32 dt) noexcept override {
//           m_Social->Tick(dt);  // callback pump
//       }
//       void OnBossKilled() noexcept {
//           (void)m_Social->UnlockAchievement("ACH_BOSS_01");
//       }
//   };
//
// 設計選択 (Pillar S):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: Steamworks SDK は static lib 配布で
//     依存追加が重い (~30MB / Win+Linux 別ライブラリ)。本体は SDK 非依存のままで
//     ビルドできるよう、ヘッダだけは常に提供し、実装は別モジュール (将来の
//     `acs_steamworks` 等) で `ISteamworksBridge` を override する形を取る。
//   ・**cross-platform で同じ I/F**: Steam / EOS / PS / Xbox のいずれを後ろで使っても
//     ゲーム側コードを書き換えない。プラットフォーム固有 ID は `const char*
//     platform_id` の opaque な文字列として渡す (例: SteamID64 を "76561198..." の
//     decimal 文字列で)。
//   ・**所有しない const char*** : 文字列は呼び出し側 / プラットフォーム SDK の
//     ライフタイムに従う。Bridge はコピーしない (STL <string> 不使用方針)。
//     利用側は GetLocalPlayer() の戻り値を「ティック内のみ有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は Init() のみ
//     成功を返し、他は ACS_ERR(Generic, kSubNotImplemented, "...") を返す。
//   ・**Tick(dt) は必須**: Steamworks の `SteamAPI_RunCallbacks()` 相当を Bridge 側に
//     畳み込む。ゲーム側は dt を毎フレーム渡すだけで、コールバックポンプの存在を
//     意識しなくて良い。
//   ・**Stub は static singleton で取得**: 依存ゼロのデフォルト実装として
//     `SteamworksBridgeStub::GetStub()` を提供。実 SDK 未統合のビルドでも
//     `m_Social = &SteamworksBridgeStub::GetStub();` だけでコンパイル可能。
//   ・**実 SDK 実装はここでは作らない**: GoldenSteamworksBridge 等は Steamworks SDK
//     ヘッダ / ライブラリへの依存を伴うため、本ファイルでは I/F + Stub のみ。
//
// 範囲外:
//   ・マルチプレイ招待。
//   ・非同期 result の Future / コールバック登録。今は同期 API 前提 (Leaderboard
//     get は SDK 側でキャッシュされた値を即返す形を想定)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * Stub が未実装機能で返すエラーサブコード (ErrCategory::Generic 配下)。
 *
 * @details
 * 本来は ErrCategory に NotImplemented を足したいが Foundation の enum 変更は影響が
 * 広いため、Generic + 安定 subcode で表現する。利用側は
 * `err.subcode == kSubSteamworksNotImplemented` でフィルタできる。
 */
inline constexpr u16 kSubSteamworksNotImplemented = 1001;

/** Init() 前に API が呼ばれたことを示すエラーサブコード (ErrCategory::Generic 配下)。 */
inline constexpr u16 kSubSteamworksNotInitialized = 1002;

/**
 * cross-platform 共通のローカルプレイヤー / フレンド最小情報。
 *
 * @details
 * Bridge は文字列を所有しない。platform_id / display_name は実 SDK 側 (または Stub 内
 * static literal) のメモリを参照するだけで、呼び出し側でコピーしない。寿命は「次の
 * Tick() を呼ぶまで」を保証する (実装によってはそれより長い)。
 */
struct PlayerIdentity {
    /** "76561198..." 等の SDK 固有 ID 文字列 (所有しない)。 */
    const char* platform_id   = nullptr;

    /** ユーザー表示名 (UTF-8、所有しない)。 */
    const char* display_name  = nullptr;

    /** SDK 発行のセッショントークン (0 = 無効)。 */
    u64         session_token = 0;
};

/**
 * Steam / EOS / PS / Xbox の差を吸収する純粋仮想シーム I/F。
 *
 * @details
 * ゲーム側コードはこの I/F 経由でのみ実績解除 / リーダーボード / ローカルプレイヤー情報
 * を取得し、実 SDK との結合はビルド時の選択で差し替える。実装は本体外モジュール
 * (or テスト) で override する。
 */
class ISteamworksBridge {
public:
    /** 抽象基底を構築する (派生で実 SDK を結線)。 */
    ISteamworksBridge() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~ISteamworksBridge() noexcept = default;

    /** コピー禁止 (Bridge は単独所有して DI するため)。 */
    ISteamworksBridge(const ISteamworksBridge&)            = delete;

    /** コピー代入も禁止。 */
    ISteamworksBridge& operator=(const ISteamworksBridge&) = delete;

    /** ムーブ禁止。 */
    ISteamworksBridge(ISteamworksBridge&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ISteamworksBridge& operator=(ISteamworksBridge&&)      = delete;

    /**
     * SDK を初期化する。
     *
     * @details 多重呼び出し可否は実装依存だが、Stub は何度呼んでも成功を返す。
     * @return 成功なら空の TResult、失敗なら SDK 固有のエラー。
     */
    virtual TResult<void> Init() noexcept = 0;

    /** SDK の終了処理を行う (Init() 前に呼んでも安全な no-op)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * 初期化済みかを返す。
     *
     * @return Init() 成功後かつ Shutdown() 前なら true。
     */
    virtual bool IsInitialized() const noexcept = 0;

    /**
     * ローカルプレイヤー情報を返す。
     *
     * @return ローカルプレイヤーの PlayerIdentity (未初期化時は空)。
     */
    virtual PlayerIdentity GetLocalPlayer() const noexcept = 0;

    /**
     * 実績を解除する。
     *
     * @details 既に解除済みの場合の振る舞いは SDK 依存 (大抵は no-op で成功)。
     * @param ach_id プラットフォームの実績 ID 文字列 ("ACH_BOSS_01" 等)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> UnlockAchievement(const char* ach_id) noexcept = 0;

    /**
     * リーダーボードへスコアを送信する。
     *
     * @details 既存値より低い値を上書きするかは SDK 側の「上書きポリシー」設定に依存する。
     * @param board_id 文字列キーのリーダーボード ID。
     * @param score 送信するスコア。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SetLeaderboardScore(const char* board_id, i64 score) noexcept = 0;

    /**
     * 自分の現在スコアを取得する。
     *
     * @param board_id 文字列キーのリーダーボード ID。
     * @return スコア (未投稿時は 0 / エラーのいずれか、実装依存)。
     */
    virtual TResult<i64> GetLeaderboardScore(const char* board_id) noexcept = 0;

    /**
     * コールバックポンプを進める (Steamworks の SteamAPI_RunCallbacks() 相当)。
     *
     * @param dt 実時間秒 (実装によっては使わない)。
     */
    virtual void Tick(f32 dt) noexcept = 0;

    /**
     * 整数 stat を設定する。
     *
     * @details 値は SDK 側で永続化される (Steamworks では Stats Configuration の Stat ID と紐づく)。
     * @param stat_name 事前定義した key 名 ("kills"、"play_minutes" 等)。
     * @param value 設定する整数値。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>   SetStat(const char* stat_name, i64 value) noexcept = 0;

    /**
     * 整数 stat を取得する。
     *
     * @param stat_name 事前定義した key 名。
     * @return stat の整数値。
     */
    virtual TResult<i64>    GetStat(const char* stat_name) noexcept = 0;

    /**
     * 浮動小数 stat を設定する。
     *
     * @param stat_name 事前定義した key 名。
     * @param value 設定する浮動小数値。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>   SetFloatStat(const char* stat_name, f32 value) noexcept = 0;

    /**
     * 浮動小数 stat を取得する。
     *
     * @param stat_name 事前定義した key 名。
     * @return stat の浮動小数値。
     */
    virtual TResult<f32>    GetFloatStat(const char* stat_name) noexcept = 0;

    /**
     * DLC が所有されているかを返す。
     *
     * @param app_id DLC の AppID (Steamworks では u32)。
     * @return 所有していれば true。
     */
    virtual bool IsDlcOwned(u32 app_id) const noexcept = 0;

    /**
     * RichPresence の key/value をセットする (Steamworks では ISteamFriends::SetRichPresence)。
     *
     * @details key="status" の value はフレンド一覧の "今プレイ中" 行に表示される。
     * @param key RichPresence のキー。
     * @param value 設定する値。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SetRichPresence(const char* key, const char* value) noexcept = 0;

    /**
     * 自分のフレンド数を返す。
     *
     * @return フレンド数 (未初期化 / 未取得時は 0)。
     */
    virtual u32 GetFriendCount() const noexcept = 0;

    /**
     * index 番目のフレンド情報を返す。
     *
     * @details 戻り値の文字列寿命は GetLocalPlayer と同じ "次の Tick まで" 契約。
     * @param index 0 <= index < GetFriendCount() のフレンドインデックス。
     * @return フレンドの PlayerIdentity (範囲外は空)。
     */
    virtual PlayerIdentity GetFriendByIndex(u32 index) const noexcept = 0;

    /**
     * Cloud (Steam Remote Storage) にファイルを書き込む。
     *
     * @details path は SDK 内部 namespace なのでパス区切り '/' でフラットに扱われる。
     * @param path Cloud 内のファイルパス。
     * @param data 書き込むデータ (寿命は関数内のみ、SDK 側がコピー)。
     * @param size data の byte 数。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>  CloudWriteFile(const char* path, const void* data, u32 size) noexcept = 0;

    /**
     * Cloud からファイルを読み出す。
     *
     * @details buf_size がファイルサイズ未満なら part 読み (末尾切り捨て)。
     * @param path Cloud 内のファイルパス。
     * @param out_buf 呼出側が用意した読み出し先バッファ。
     * @param buf_size out_buf の容量。
     * @return 読み出した byte 数 (= ファイルサイズ)。0 は「ファイル無し」or エラー。
     */
    virtual TResult<u32>   CloudReadFile(const char* path, void* out_buf, u32 buf_size) noexcept = 0;

    /**
     * Cloud にファイルが存在するかを返す。
     *
     * @param path Cloud 内のファイルパス。
     * @return 存在すれば true。
     */
    virtual bool           CloudFileExists(const char* path) const noexcept = 0;

    /**
     * Cloud からファイルを削除する。
     *
     * @details 存在しない path に対しては成功扱い (idempotent)。
     * @param path Cloud 内のファイルパス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>  CloudDeleteFile(const char* path) noexcept = 0;

    /**
     * Cloud の quota 情報を取得する。
     *
     * @param out_available_bytes 空き容量の出力先 (エラー時は 0)。
     * @param out_total_bytes 総容量の出力先 (エラー時は 0)。
     */
    virtual void           CloudGetQuota(u64& out_available_bytes, u64& out_total_bytes) const noexcept = 0;

    /**
     * サブスクライブ済み Workshop アイテム数を返す。
     *
     * @return サブスクライブ済みアイテム数。
     */
    virtual u32            WorkshopGetSubscribedCount() const noexcept = 0;

    /**
     * index 番目のサブスクライブ済み Workshop アイテムを取得する。
     *
     * @param index アイテムのインデックス。
     * @param out_item_id アイテム ID の出力先 (未取得 / 範囲外は 0)。
     * @param out_install_path インストール path の出力先 (SDK 内 buffer、寿命は次の Tick まで。範囲外は nullptr)。
     */
    virtual void           WorkshopGetSubscribedItem(u32 index, u64& out_item_id,
                                                    const char*& out_install_path) const noexcept = 0;

    /**
     * ボイスチャットの録音を開始する (push-to-talk 想定)。
     *
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>  VoiceStartRecording() noexcept = 0;

    /**
     * ボイスチャットの録音を停止する。
     *
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>  VoiceStopRecording()  noexcept = 0;

    /**
     * 圧縮音声データを取得する (16-bit PCM compressed)。
     *
     * @param out_buf 呼出側が用意した取得先バッファ。
     * @param buf_size out_buf の容量。
     * @return 実際に取得した byte 数 (0 は「データなし」)。
     */
    virtual TResult<u32>   VoiceGetCompressed(void* out_buf, u32 buf_size) noexcept = 0;

    /**
     * Steam Input を初期化する (game action sets を SDK に通知)。
     *
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void>  InputInit() noexcept = 0;

    /**
     * 接続中のコントローラ数を返す (XInput / DualSense 等を Steam Input が抽象化)。
     *
     * @return 接続コントローラ数。
     */
    virtual u32            InputGetControllerCount() const noexcept = 0;
};

/**
 * Steamworks SDK 未統合ビルド / ユニットテスト用の no-op 実装。
 *
 * @details
 * Init() のみ常に成功し、GetLocalPlayer() は固定ダミー ("stub_player" / "Player") を
 * 返す。Achievement / Leaderboard 等の機能系は ACS_ERR(Generic,
 * kSubSteamworksNotImplemented) を返し、Shutdown() / Tick() は副作用を持たない。
 */
class SteamworksBridgeStub final : public ISteamworksBridge {
public:
    /** 未初期化状態の Stub を構築する。 */
    SteamworksBridgeStub() noexcept = default;

    /** Stub を破棄する (副作用なし)。 */
    ~SteamworksBridgeStub() noexcept override = default;

    /**
     * 初期化済みフラグを立てて常に成功を返す。
     *
     * @return 常に空の成功 TResult。
     */
    TResult<void>    Init() noexcept override;

    /** 初期化済みフラグを下ろす (no-op に近い後始末)。 */
    void            Shutdown() noexcept override;

    /**
     * 初期化済みかを返す。
     *
     * @return Init() 後かつ Shutdown() 前なら true。
     */
    bool            IsInitialized() const noexcept override { return m_Initialized; }

    /**
     * 固定ダミーのローカルプレイヤー情報を返す。
     *
     * @return platform_id="stub_player" / display_name="Player" の PlayerIdentity。
     */
    PlayerIdentity  GetLocalPlayer() const noexcept override;

    /**
     * 実績解除を試みる (未実装)。
     *
     * @param ach_id 実績 ID 文字列 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    UnlockAchievement(const char* ach_id) noexcept override;

    /**
     * リーダーボードへスコア送信を試みる (未実装)。
     *
     * @param board_id リーダーボード ID (Stub では未使用)。
     * @param score 送信するスコア (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    SetLeaderboardScore(const char* board_id, i64 score) noexcept override;

    /**
     * リーダーボードのスコア取得を試みる (未実装)。
     *
     * @param board_id リーダーボード ID (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<i64>     GetLeaderboardScore(const char* board_id) noexcept override;

    /**
     * コールバックポンプを進める (Stub では副作用なし)。
     *
     * @param dt 実時間秒 (Stub では未使用)。
     */
    void            Tick(f32 dt) noexcept override;

    /**
     * 整数 stat の設定を試みる (未実装)。
     *
     * @param stat_name stat の key 名 (Stub では未使用)。
     * @param value 設定値 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    SetStat(const char* stat_name, i64 value) noexcept override;

    /**
     * 整数 stat の取得を試みる (未実装)。
     *
     * @param stat_name stat の key 名 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<i64>     GetStat(const char* stat_name) noexcept override;

    /**
     * 浮動小数 stat の設定を試みる (未実装)。
     *
     * @param stat_name stat の key 名 (Stub では未使用)。
     * @param value 設定値 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    SetFloatStat(const char* stat_name, f32 value) noexcept override;

    /**
     * 浮動小数 stat の取得を試みる (未実装)。
     *
     * @param stat_name stat の key 名 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<f32>     GetFloatStat(const char* stat_name) noexcept override;

    /**
     * DLC 所有を返す (Stub では常に未所有)。
     *
     * @param app_id DLC の AppID (Stub では未使用)。
     * @return 常に false。
     */
    bool            IsDlcOwned(u32 app_id) const noexcept override;

    /**
     * RichPresence の設定を試みる (未実装)。
     *
     * @param key RichPresence のキー (Stub では未使用)。
     * @param value 設定値 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    SetRichPresence(const char* key, const char* value) noexcept override;

    /**
     * フレンド数を返す (Stub では常に 0)。
     *
     * @return 常に 0。
     */
    u32             GetFriendCount() const noexcept override;

    /**
     * index 番目のフレンド情報を返す (Stub では空)。
     *
     * @param index フレンドインデックス (Stub では未使用)。
     * @return 空の PlayerIdentity。
     */
    PlayerIdentity  GetFriendByIndex(u32 index) const noexcept override;

    /**
     * Cloud へのファイル書き込みを試みる (未実装)。
     *
     * @param path Cloud 内のファイルパス (Stub では未使用)。
     * @param data 書き込むデータ (Stub では未使用)。
     * @param size data の byte 数 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    CloudWriteFile(const char* path, const void* data, u32 size) noexcept override;

    /**
     * Cloud からのファイル読み出しを試みる (未実装)。
     *
     * @param path Cloud 内のファイルパス (Stub では未使用)。
     * @param out_buf 読み出し先バッファ (Stub では未使用)。
     * @param buf_size out_buf の容量 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<u32>     CloudReadFile(const char* path, void* out_buf, u32 buf_size) noexcept override;

    /**
     * Cloud のファイル存在を返す (Stub では常に false)。
     *
     * @param path Cloud 内のファイルパス (Stub では未使用)。
     * @return 常に false。
     */
    bool            CloudFileExists(const char* path) const noexcept override;

    /**
     * Cloud のファイル削除を試みる (未実装)。
     *
     * @param path Cloud 内のファイルパス (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    CloudDeleteFile(const char* path) noexcept override;

    /**
     * Cloud の quota を返す (Stub では 0 を出力)。
     *
     * @param out_available_bytes 空き容量の出力先 (常に 0)。
     * @param out_total_bytes 総容量の出力先 (常に 0)。
     */
    void            CloudGetQuota(u64& out_available_bytes, u64& out_total_bytes) const noexcept override;

    /**
     * サブスクライブ済み Workshop アイテム数を返す (Stub では常に 0)。
     *
     * @return 常に 0。
     */
    u32             WorkshopGetSubscribedCount() const noexcept override;

    /**
     * index 番目の Workshop アイテムを返す (Stub では空)。
     *
     * @param index アイテムのインデックス (Stub では未使用)。
     * @param out_item_id アイテム ID の出力先 (常に 0)。
     * @param out_install_path インストール path の出力先 (常に nullptr)。
     */
    void            WorkshopGetSubscribedItem(u32 index, u64& out_item_id,
                                              const char*& out_install_path) const noexcept override;

    /**
     * ボイス録音開始を試みる (未実装)。
     *
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    VoiceStartRecording() noexcept override;

    /**
     * ボイス録音停止を試みる (未実装)。
     *
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    VoiceStopRecording() noexcept override;

    /**
     * 圧縮音声データの取得を試みる (未実装)。
     *
     * @param out_buf 取得先バッファ (Stub では未使用)。
     * @param buf_size out_buf の容量 (Stub では未使用)。
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<u32>     VoiceGetCompressed(void* out_buf, u32 buf_size) noexcept override;

    /**
     * Steam Input 初期化を試みる (未実装)。
     *
     * @return ACS_ERR(Generic, kSubSteamworksNotImplemented)。
     */
    TResult<void>    InputInit() noexcept override;

    /**
     * 接続コントローラ数を返す (Stub では常に 0)。
     *
     * @return 常に 0。
     */
    u32             InputGetControllerCount() const noexcept override;

    /**
     * 全コードで共有できる static singleton を返す。
     *
     * @details 実 SDK 実装が DI される前のデフォルト Bridge として使う。
     * @return プロセス唯一の Stub インスタンスへの参照。
     */
    static SteamworksBridgeStub& GetStub() noexcept;

private:
    /** 初期化済みフラグ (Init で true、Shutdown で false)。 */
    bool m_Initialized = false;
};

/**
 * 既定 Bridge を返す provider 関数ポインタ型。
 *
 * @details
 * gameframework は実 backend モジュール (ACS::Steamworks / FSteamworksBridgeImpl) に
 * 依存できない (backend 側が本 interface に依存するため循環になる)。そこで実 backend
 * 側がこの型の関数を SetSteamworksBridgeProvider で登録し、ゲームコードは
 * GetDefaultSteamworksBridge を通じて backend 非依存に既定 Bridge を取得する。
 */
using SteamworksBridgeProvider = ISteamworksBridge& (*)() noexcept;

/**
 * 既定 Bridge provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details nullptr を登録すると Stub に戻る。複数回登録した場合は後勝ち。
 * @param provider 既定 Bridge を返す関数ポインタ (nullptr で Stub にリセット)。
 */
void SetSteamworksBridgeProvider(SteamworksBridgeProvider provider) noexcept;

/**
 * 既定 ISteamworksBridge を返す。
 *
 * @return provider 登録済みならその実 Bridge、未登録なら SteamworksBridgeStub::GetStub()。
 */
ISteamworksBridge& GetDefaultSteamworksBridge() noexcept;

} // namespace acs::game
