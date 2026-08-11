// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * Stub が未実装 API で返す subcode (EErrCategory::Generic 配下)。
 *
 * @details UGC bridge の未実装状態を Generic category の 1100 番台で識別する。
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
 * Bridge は文字列を所有しない。title / description / author は具象 backend 側
 * (または Stub 内 static literal) のメモリを参照し、寿命は「次の Tick() を
 * 呼ぶまで」を保証する (実装によってはそれより長い)。
 */
struct FWorkshopItem {
    /** UGC backend が発行する opaque ID。 */
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
 * UGC の publish / subscribe / download を抽象化する純粋仮想インターフェース。
 *
 * @details
 * 具象 UGC backend の差を吸収し、ゲーム側は本インターフェース経由で UGC の
 * publish / subscribe / download を行う。u64 item_id は backend 固有 ID を表す
 * opaque key として扱う。
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
     * @details subscribe 後の自動 download 開始有無は具象 backend が決定する。
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
     * 1 frame 分の callback と provider 状態を更新する。
     *
     * @details provider 固有の callback と状態更新を Bridge 側にまとめる。
     * @param dt 前フレームからの経過秒。
     */
    virtual void Tick(f32 dt) noexcept = 0;
};

/**
 * 具象 UGC backend 未接続時に使用する no-op 実装。
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
 * 具象 backend が登録される前の既定値として、thread-safe に初期化される
 * process 共有 Stub を自由関数で公開する。
 * @return プロセス内で唯一の CWorkshopBridgeStub への参照。
 */
IWorkshopBridge& GetWorkshopStub() noexcept;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWorkshopBridgeStub = CWorkshopBridgeStub;

} // namespace acs::game
