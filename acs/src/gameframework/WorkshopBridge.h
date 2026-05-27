// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FWorkshopBridge (Steam Workshop / UGC seam)
//
// Steam Workshop / EOS UGC / mod.io といったプラットフォーム固有 UGC SDK へ橋渡し
// するシーム (seam) インターフェース。ゲーム側コードは IWorkshopBridge 経由でのみ
// ユーザー生成コンテンツ (UGC) の publish / subscribe / download を行い、実 SDK
// (Steamworks UGC API 等) との結合はビルド時の選択で差し替える。
//
// 使い方:
//   class ModBrowserUI {
//       acs::game::IWorkshopBridge* _workshop = nullptr;
//
//       void OnStart() noexcept override {
//           _workshop = &acs::game::GetWorkshopStub();
//           (void)_workshop->Init();
//       }
//       void OnTick(f32 dt) noexcept override {
//           _workshop->Tick(dt);  // callback pump
//       }
//       void OnSubscribeClicked(acs::u64 item_id) noexcept {
//           (void)_workshop->SubscribeItem(item_id);
//           (void)_workshop->DownloadItem(item_id);
//       }
//   };
//
// 設計選択 (Pillar S Phase 2):
//   ・**FSteamworksBridge とは別 I/F**: Workshop は publish ワークフロー (CreateItem /
//     UpdateItem) を持ち、API 面が Achievement / Leaderboard とは別領域。一緒くたに
//     すると Bridge が肥大化するため、別ファイルで隔離する。
//   ・**`u64 item_id` を opaque key として扱う**: Steamworks では PublishedFileId_t
//     (= u64)、mod.io では mod ID (= u32 だが u64 上位ビット 0 で表現可) を共通化。
//   ・**所有しない const char***: 文字列は FSteamworksBridge と同じく呼び出し側 /
//     プラットフォーム SDK のライフタイムに従い、Bridge はコピーしない。
//     QueryItem() の戻り値は「次の Tick() を呼ぶまで有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は Init() のみ
//     成功、その他は ACS_ERR(Generic, kSubWorkshopNotImplemented, ...) を返す。
//   ・**ダウンロード進捗は同期 poll**: GetDownloadProgress(item_id) を毎フレーム
//     呼んで [0, 1] を取得。-1 は「現在ダウンロード中ではない / 不明」を表す。
//     非同期 callback 登録は Phase 3+ で検討。
//   ・**Stub は static singleton で取得**: FSteamworksBridge と同じく
//     `GetWorkshopStub()` を提供。`WorkshopBridgeStub::IsAvailable()` は
//     常に false を返し、UI 側で「Workshop 機能無効」表示の判定に使える。
//   ・**実 SDK 実装はここでは作らない**: GoldenWorkshopBridge 等は Steamworks UGC
//     API への依存を伴うため、本ファイルでは I/F + Stub のみ。
//
// 範囲外 (Phase 3+ で):
//   ・タグ / カテゴリ検索、評価 (vote up/down)、コメント。
//   ・依存関係 (item A が item B に依存)、コレクション。
//   ・非同期 callback 登録、Future 形式の API。
//   ・プレビュー画像 / 動画のアップロード。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- FErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// FSteamworksBridge と subcode 空間が重ならないよう 1100 番台を使う。
inline constexpr u16 kSubWorkshopNotImplemented = 1101;  // Stub による未実装
inline constexpr u16 kSubWorkshopNotInitialized = 1102;  // Init() 前の API 呼び出し
inline constexpr u16 kSubWorkshopUnavailable    = 1103;  // SDK が無効 (Stub 等)

// ---- WorkshopItem (cross-platform 共通の UGC メタ情報) -------------------
// Bridge は文字列を所有しない。`title` / `description` / `author` は実 SDK 側
// (または Stub 内 static literal) のメモリを参照する。寿命は「次の Tick() を
// 呼ぶまで」を保証する (実装によってはそれより長い)。
struct WorkshopItem {
    u64         item_id     = 0;        // SDK 固有の opaque ID (Steam PublishedFileId_t 等)
    const char* title       = nullptr;  // ユーザー表示タイトル (UTF-8)
    const char* description = nullptr;  // 説明文 (UTF-8、長文可)
    const char* author      = nullptr;  // 投稿者表示名 (UTF-8)
    u64         file_size   = 0;        // ダウンロード後のファイルサイズ (バイト)
    bool        installed   = false;    // ローカルにダウンロード済みなら true
    bool        subscribed  = false;    // ローカルプレイヤーが subscribe 中なら true
};

// ---- 抽象 I/F -------------------------------------------------------------
// Steam Workshop / EOS UGC / mod.io の差を吸収する純粋仮想インターフェース。
// 実装は本体外モジュール (or テスト) で Override する。
class IWorkshopBridge {
public:
    IWorkshopBridge() noexcept = default;
    virtual ~IWorkshopBridge() noexcept = default;

    IWorkshopBridge(const IWorkshopBridge&)            = delete;
    IWorkshopBridge& operator=(const IWorkshopBridge&) = delete;
    IWorkshopBridge(IWorkshopBridge&&)                 = delete;
    IWorkshopBridge& operator=(IWorkshopBridge&&)      = delete;

    // SDK 初期化。失敗は SDK 固有のエラーを TResult で返す。多重呼び出し可否は
    // 実装依存だが、Stub は何度呼んでも成功を返す。
    virtual TResult<void> Init() noexcept = 0;

    // SDK 終了処理。Init() 前に呼んでも安全 (no-op)。
    virtual void Shutdown() noexcept = 0;

    // Workshop / UGC 機能が現在のビルドで利用可能か。Stub は常に false を返し、
    // UI 側で「Workshop 機能はこのビルドでは無効」と表示する判定に使う。
    virtual bool IsAvailable() const noexcept = 0;

    // 新規 UGC アイテムの publish。content_path はローカルファイル / ディレクトリ
    // のパス (UTF-8)。成功時は SDK が割り当てた item_id を返す。
    virtual TResult<u64> CreateItem(const char* title, const char* content_path) noexcept = 0;

    // 既存アイテムの更新 (差分アップロード)。change_note は変更履歴に表示される。
    virtual TResult<void> UpdateItem(u64 item_id,
                                    const char* content_path,
                                    const char* change_note) noexcept = 0;

    // アイテムメタ情報のクエリ。戻り値の文字列は次の Tick() まで有効。
    virtual TResult<WorkshopItem> QueryItem(u64 item_id) noexcept = 0;

    // ローカルプレイヤーが subscribe 中のアイテム数。
    virtual TResult<u32> QuerySubscribedCount() noexcept = 0;

    // アイテムを subscribe する。SDK 側で自動的にダウンロードが始まる場合もある
    // (Steam の標準挙動)。
    virtual TResult<void> SubscribeItem(u64 item_id) noexcept = 0;

    // subscribe を解除。ローカルファイルは SDK 側で削除される場合もある。
    virtual TResult<void> UnsubscribeItem(u64 item_id) noexcept = 0;

    // 明示的なダウンロード開始 (subscribe 済みアイテムを再ダウンロード等)。
    virtual TResult<void> DownloadItem(u64 item_id) noexcept = 0;

    // ダウンロード進捗を [0, 1] で返す。完了済み / 未開始 / 不明な場合は -1。
    // ポーリング前提のシンプル API (Phase 3+ で非同期 callback に置き換え予定)。
    virtual f32 GetDownloadProgress(u64 item_id) noexcept = 0;

    // コールバックポンプ。SteamAPI_RunCallbacks() 相当を Bridge 側に畳み込む。
    // ゲームループから毎フレーム呼ぶこと。
    virtual void Tick(f32 dt) noexcept = 0;
};

// ---- Stub 実装 ------------------------------------------------------------
// Steamworks UGC SDK 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・Init() のみ常に成功 (_initialized = true)。
//   ・IsAvailable() は常に false (UI 側で Workshop ボタンを非表示にする判定用)。
//   ・全 publish / subscribe / download 系は ACS_ERR(Generic,
//     kSubWorkshopNotImplemented) を返す。
//   ・GetDownloadProgress() は常に -1 を返す。
//   ・Shutdown() / Tick() は副作用なし。
class WorkshopBridgeStub final : public IWorkshopBridge {
public:
    WorkshopBridgeStub() noexcept = default;
    ~WorkshopBridgeStub() noexcept override = default;

    TResult<void>         Init() noexcept override;
    void                 Shutdown() noexcept override;
    bool                 IsAvailable() const noexcept override { return false; }
    TResult<u64>          CreateItem(const char* title, const char* content_path) noexcept override;
    TResult<void>         UpdateItem(u64 item_id, const char* content_path, const char* change_note) noexcept override;
    TResult<WorkshopItem> QueryItem(u64 item_id) noexcept override;
    TResult<u32>          QuerySubscribedCount() noexcept override;
    TResult<void>         SubscribeItem(u64 item_id) noexcept override;
    TResult<void>         UnsubscribeItem(u64 item_id) noexcept override;
    TResult<void>         DownloadItem(u64 item_id) noexcept override;
    f32                  GetDownloadProgress(u64 item_id) noexcept override;
    void                 Tick(f32 dt) noexcept override;

private:
    bool _initialized = false;
};

// 全コードで共有できる static singleton。実 SDK 実装が DI される前のデフォルト。
// FSteamworksBridge では `SteamworksBridgeStub::GetStub()` という静的メンバ
// 関数を使っているが、Workshop 側は仕様に合わせて自由関数で公開する
// (どちらも Meyer's singleton で thread-safe)。
IWorkshopBridge& GetWorkshopStub() noexcept;

} // namespace acs::game
