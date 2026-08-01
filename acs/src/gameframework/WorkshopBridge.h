// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — IWorkshopBridge (Steam Workshop / UGC seam)
//
// Steam Workshop / EOS UGC / mod.io といったプラットフォーム固有 UGC SDK へ橋渡し
// するシーム (seam) インターフェース。ゲーム側コードは IWorkshopBridge 経由でのみ
// ユーザー生成コンテンツ (UGC) の publish / subscribe / download を行い、実 SDK
// (Steamworks UGC API 等) との結合はビルド時の選択で差し替える。
//
// 使い方:
//   class FModBrowserUi {
//       acs::game::IWorkshopBridge* m_Workshop = nullptr;
//
//       void OnStart() noexcept override {
//           m_Workshop = &acs::game::GetWorkshopStub();
//           (void)m_Workshop->Init();
//       }
//       void OnTick(f32 dt) noexcept override {
//           m_Workshop->Tick(dt);  // callback pump
//       }
//       void OnSubscribeClicked(acs::u64 item_id) noexcept {
//           (void)m_Workshop->SubscribeItem(item_id);
//           (void)m_Workshop->DownloadItem(item_id);
//       }
//   };
//
// 設計選択:
//   ・**ISteamworksBridge とは別 I/F**: Workshop は publish ワークフロー (CreateItem /
//     UpdateItem) を持ち、API 面が Achievement / Leaderboard とは別領域。一緒くたに
//     すると Bridge が肥大化するため、別ファイルで隔離する。
//   ・**`u64 item_id` を opaque key として扱う**: Steamworks では PublishedFileId_t
//     (= u64)、mod.io では mod ID (= u32 だが u64 上位ビット 0 で表現可) を共通化。
//   ・**所有しない const char***: 文字列は ISteamworksBridge と同じく呼び出し側 /
//     プラットフォーム SDK のライフタイムに従い、Bridge はコピーしない。
//     QueryItem() の戻り値は「次の Tick() を呼ぶまで有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は Init() のみ
//     成功、その他は ACS_ERR(Generic, kSubWorkshopNotImplemented, ...) を返す。
//   ・**ダウンロード進捗は同期 poll**: GetDownloadProgress(item_id) を毎フレーム
//     呼んで [0, 1] を取得。-1 は「現在ダウンロード中ではない / 不明」を表す。
//   ・**Stub は static singleton で取得**: ISteamworksBridge と同じく
//     `GetWorkshopStub()` を提供。`CWorkshopBridgeStub::IsAvailable()` は
//     常に false を返し、UI 側で「Workshop 機能無効」表示の判定に使える。
//   ・**実 SDK 実装はここでは作らない**: GoldenWorkshopBridge 等は Steamworks UGC
//     API への依存を伴うため、本ファイルでは I/F + Stub のみ。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * Stub が未実装 API で返す subcode (EErrCategory::Generic 配下)。
 *
 * @details ISteamworksBridge と subcode 空間が重ならないよう 1100 番台を使う。
 */
inline constexpr u16 kSubWorkshopNotImplemented = 1101;

/** Init() 前に API を呼び出したときの subcode。 */
inline constexpr u16 kSubWorkshopNotInitialized = 1102;

/** SDK が無効 (Stub 等) のときの subcode。 */
inline constexpr u16 kSubWorkshopUnavailable    = 1103;

/**
 * cross-platform 共通の UGC アイテムメタ情報。
 *
 * @details
 * Bridge は文字列を所有しない。title / description / author は実 SDK 側
 * (または Stub 内 static literal) のメモリを参照し、寿命は「次の Tick() を
 * 呼ぶまで」を保証する (実装によってはそれより長い)。
 */
struct FWorkshopItem {
    /** SDK 固有の opaque ID (Steam PublishedFileId_t 等)。 */
    u64         item_id     = 0;

    /** ユーザー表示タイトル (UTF-8、所有しない)。 */
    const char* title       = nullptr;

    /** 説明文 (UTF-8、長文可、所有しない)。 */
    const char* description = nullptr;

    /** 投稿者表示名 (UTF-8、所有しない)。 */
    const char* author      = nullptr;

    /** ダウンロード後のファイルサイズ (バイト)。 */
    u64         file_size   = 0;

    /** ローカルにダウンロード済みなら true。 */
    bool        installed   = false;

    /** ローカルプレイヤーが subscribe 中なら true。 */
    bool        subscribed  = false;
};

/**
 * Steam Workshop / EOS UGC / mod.io への橋渡しシーム (純粋仮想 I/F)。
 *
 * @details
 * プラットフォーム固有 UGC SDK の差を吸収し、ゲーム側は本 I/F 経由でのみ UGC の
 * publish / subscribe / download を行う。実装は本体外モジュール (or テスト) で
 * Override する。u64 item_id は SDK 固有 ID を表す opaque key として扱う。
 */
class IWorkshopBridge {
public:
    /** 空のブリッジを構築する。 */
    IWorkshopBridge() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IWorkshopBridge() noexcept = default;

    /** コピー禁止 (ブリッジは参照で扱う seam のため)。 */
    IWorkshopBridge(const IWorkshopBridge&)            = delete;

    /** コピー代入も禁止。 */
    IWorkshopBridge& operator=(const IWorkshopBridge&) = delete;

    /** ムーブ禁止。 */
    IWorkshopBridge(IWorkshopBridge&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IWorkshopBridge& operator=(IWorkshopBridge&&)      = delete;

    /**
     * SDK を初期化する。
     *
     * @details
     * 失敗は SDK 固有のエラーを TResult で返す。多重呼び出し可否は実装依存だが、
     * Stub は何度呼んでも成功を返す。
     * @return 成功なら空の TResult、失敗なら SDK 固有のエラー。
     */
    virtual TResult<void> Init() noexcept = 0;

    /** SDK の終了処理を行う (Init() 前に呼んでも安全な no-op)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * Workshop / UGC 機能が現在のビルドで利用可能かを返す。
     *
     * @details Stub は常に false を返し、UI 側で「Workshop 機能はこのビルドでは無効」と表示する判定に使う。
     * @return 利用可能なら true。
     */
    virtual bool IsAvailable() const noexcept = 0;

    /**
     * 新規 UGC アイテムを publish する。
     *
     * @param title アイテムのユーザー表示タイトル (UTF-8)。
     * @param content_path アップロードするローカルファイル / ディレクトリのパス (UTF-8)。
     * @return 成功なら SDK が割り当てた item_id、失敗ならエラー。
     */
    virtual TResult<u64> CreateItem(const char* title, const char* content_path) noexcept = 0;

    /**
     * 既存アイテムを更新する (差分アップロード)。
     *
     * @param item_id 更新対象アイテムの opaque ID。
     * @param content_path 更新後のローカルファイル / ディレクトリのパス (UTF-8)。
     * @param change_note 変更履歴に表示される変更メモ (UTF-8)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> UpdateItem(u64 item_id,
                                    const char* content_path,
                                    const char* change_note) noexcept = 0;

    /**
     * アイテムのメタ情報をクエリする。
     *
     * @param item_id クエリ対象アイテムの opaque ID。
     * @return 成功なら FWorkshopItem (内部の文字列は次の Tick() まで有効)、失敗ならエラー。
     */
    virtual TResult<FWorkshopItem> QueryItem(u64 item_id) noexcept = 0;

    /**
     * ローカルプレイヤーが subscribe 中のアイテム数を返す。
     *
     * @return 成功なら subscribe 中のアイテム数、失敗ならエラー。
     */
    virtual TResult<u32> QuerySubscribedCount() noexcept = 0;

    /**
     * アイテムを subscribe する。
     *
     * @details SDK 側で自動的にダウンロードが始まる場合もある (Steam の標準挙動)。
     * @param item_id subscribe するアイテムの opaque ID。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SubscribeItem(u64 item_id) noexcept = 0;

    /**
     * アイテムの subscribe を解除する。
     *
     * @details ローカルファイルは SDK 側で削除される場合もある。
     * @param item_id 解除するアイテムの opaque ID。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> UnsubscribeItem(u64 item_id) noexcept = 0;

    /**
     * アイテムの明示的なダウンロードを開始する (subscribe 済みアイテムの再ダウンロード等)。
     *
     * @param item_id ダウンロードするアイテムの opaque ID。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> DownloadItem(u64 item_id) noexcept = 0;

    /**
     * ダウンロード進捗を返す (ポーリング前提のシンプル API)。
     *
     * @param item_id 進捗を取得するアイテムの opaque ID。
     * @return 進捗を [0, 1] で返す。完了済み / 未開始 / 不明な場合は -1。
     */
    virtual f32 GetDownloadProgress(u64 item_id) noexcept = 0;

    /**
     * コールバックポンプを回す (ゲームループから毎フレーム呼ぶ)。
     *
     * @details SteamAPI_RunCallbacks() 相当を Bridge 側に畳み込む。
     * @param dt 前フレームからの経過秒。
     */
    virtual void Tick(f32 dt) noexcept = 0;
};

/**
 * Steamworks UGC SDK 未統合ビルド / ユニットテスト用の no-op 実装。
 *
 * @details
 * Init() のみ常に成功 (m_Initialized = true)、IsAvailable() は常に false
 * (UI 側で Workshop ボタンを非表示にする判定用)。全 publish / subscribe /
 * download / query 系は ACS_ERR(Generic, kSubWorkshopNotImplemented) を返す。
 * GetDownloadProgress() は常に -1、Shutdown() / Tick() は副作用なし。
 */
class CWorkshopBridgeStub final : public IWorkshopBridge {
public:
    /** 未初期化状態の Stub を構築する。 */
    CWorkshopBridgeStub() noexcept = default;

    /** 何もせず破棄する。 */
    ~CWorkshopBridgeStub() noexcept override = default;

    /**
     * 初期化済みフラグを立てる (常に成功)。
     *
     * @return 常に成功した空の TResult。
     */
    TResult<void>         Init() noexcept override;

    /** 初期化済みフラグを下ろす (副作用なし)。 */
    void                 Shutdown() noexcept override;

    /**
     * 常に false を返す (Workshop 機能無効)。
     *
     * @return 常に false。
     */
    bool                 IsAvailable() const noexcept override { return false; }

    /**
     * 未実装エラーを返す。
     *
     * @param title 使用しない (Stub のため)。
     * @param content_path 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<u64>          CreateItem(const char* title, const char* content_path) noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @param item_id 使用しない (Stub のため)。
     * @param content_path 使用しない (Stub のため)。
     * @param change_note 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<void>         UpdateItem(u64 item_id, const char* content_path, const char* change_note) noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @param item_id 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<FWorkshopItem> QueryItem(u64 item_id) noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<u32>          QuerySubscribedCount() noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @param item_id 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<void>         SubscribeItem(u64 item_id) noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @param item_id 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<void>         UnsubscribeItem(u64 item_id) noexcept override;

    /**
     * 未実装エラーを返す。
     *
     * @param item_id 使用しない (Stub のため)。
     * @return Init() 前なら未初期化エラー、それ以外は未実装エラー。
     */
    TResult<void>         DownloadItem(u64 item_id) noexcept override;

    /**
     * 常に -1.0f を返す (ダウンロード中ではない / 不明)。
     *
     * @param item_id 使用しない (Stub のため)。
     * @return 常に -1.0f。
     */
    f32                  GetDownloadProgress(u64 item_id) noexcept override;

    /**
     * 何もしない (Stub は callback pump を持たない)。
     *
     * @param dt 使用しない (Stub のため)。
     */
    void                 Tick(f32 dt) noexcept override;

private:
    /** 初期化済みフラグ (Init() で true、Shutdown() で false)。 */
    bool m_Initialized = false;
};

/**
 * 全コードで共有できる Stub の static singleton を返す。
 *
 * @details
 * 実 SDK 実装が DI される前のデフォルト。Meyer's singleton で構築するため
 * thread-safe。ISteamworksBridge は静的メンバ関数を使うが、Workshop 側は仕様に
 * 合わせて自由関数で公開する。
 * @return プロセス内で唯一の CWorkshopBridgeStub への参照。
 */
IWorkshopBridge& GetWorkshopStub() noexcept;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWorkshopBridgeStub = CWorkshopBridgeStub;

} // namespace acs::game
