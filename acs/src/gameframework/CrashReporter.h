// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — FCrashReporter (ship build 専用クラッシュ報告 seam)
//
// 役割:
//   出荷ビルドでプロセスが落ちた時、外部のクラッシュ集約サービス (Sentry /
//   Crashpad / Backtrace.io / BugSnag 等) へ最低限の context を吐き出すための
//   **抽象 seam**。ACS 本体は具象な HTTP/IPC スタックを抱え込まず、
//   `ICrashReporterBackend` インターフェイスと NotImplemented を返すだけの
//   `CrashReporterStub` のみを提供する。
//
//   ・タイトル側 (acs::FApplication) は ICrashReporterBackend* を持ち、
//   ・実装 (CrashReporterSentry, CrashReporterCrashpad 等) はプロジェクト個別に
//     差し込む。
//   これにより、(a) ACS Foundation/GameFramework の依存最小化、(b) ネットワーク
//   隔離環境でもリンクが通る、(c) ユニットテスト用 fake を簡単に差せる、を
//   満たす。
//
// 想定される具象実装 (本ファイルには含めない):
//   ・CrashReporterSentry      — Sentry Native SDK (sentry.io / 自己ホスト)
//   ・CrashReporterCrashpad    — Google Crashpad (out-of-process minidump)
//   ・CrashReporterBacktrace   — Backtrace.io coresnap
//   ・CrashReporterBugSnag     — BugSnag Cocoa/C++ SDK
//
// 範囲外 (本フェーズでは扱わない):
//   ・OS signal / SEH ハンドラの登録          (具象実装側の責務)
//   ・minidump / coredump の収集とアップロード (具象実装側の責務)
//   ・PII (個人情報) のフィルタ                (呼出側が breadcrumb に詰める段で守る)
//   ・symbolication (シンボル解決)            (サーバ側 / build pipeline の責務)
//
// 設計選択:
//   ・**stub interface のみ**: 本ヘッダ + .cpp は ICrashReporterBackend を
//     **抽象 interface として宣言** し、合わせて **常に NotImplemented を返す
//     CrashReporterStub** を提供するだけ。ACS 本体がリンク時に「最低 1 実装が
//     居る」を保証するための fallback。
//   ・**TResult<T, FErrorCode>**: 例外不使用方針。送信失敗・未初期化・引数不正は
//     すべて FErrorCode で伝搬。上位層は `if (r.IsErr()) { /* swallow */ }` で
//     クラッシュ報告自体の失敗を黙って握り潰すのが基本 (二次クラッシュ防止)。
//   ・**const char* 非所有**: exception_type / message / stack_trace / scene_name
//     / build_id 等はすべて呼出側が寿命を保証する static / member バッファ。
//     STL <string> 禁止方針。Backend 側はコピーが必要なら内部でやる。
//   ・**全 noexcept**: クラッシュ報告系で例外が飛ぶと二次クラッシュになるため
//     関数単位で例外境界を固定。
//   ・**SetUserId は anonymous 推奨**: GDPR / CCPA 対応のため、生の email や
//     OS account 名でなく、初回起動時に生成した UUID 等を渡すこと。
//   ・**Breadcrumb モデル**: クラッシュ前 N 件の context を時系列で残す、
//     Sentry/BugSnag 系で標準的なパターン。本 interface では単に「category +
//     message」を追加する形を提供。リングバッファの寸法等は具象実装側で持つ。
//   ・**Tick(f32 dt)**: 非同期送信キューの pump。毎フレーム呼ばれる前提で、
//     内部キューや受信スレッドからのメッセージをメインスレッドに引き上げる。
//     stub は no-op。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// 共通: stub 用エラーサブコード
// -----------------------------------------------------------------------------
// FSaveSlot / FBackendClient と同じく、本ピラーでも「Phase 1 stub = NotImplemented」
// を `subcode = kSub_NotImplemented` で表現する。`ErrCategory` には Generic を
// 使う (クラッシュ報告は I/O とは限らず Generic seam の性格が強い)。
// =============================================================================
struct CrashReporterError {
    enum SubCode : u16 {
        kSub_NotInitialized = 1,  // Init() 前の API 呼び出し
        kSub_AlreadyInited  = 2,  // 2 重 Init (実装側で許容するかは任意)
        kSub_BadArgument    = 3,  // product_id / message が nullptr 等
        kSub_QueueFull      = 4,  // 内部キュー溢れ (breadcrumb / event)
        kSub_NetworkFailure = 5,  // アップロード失敗
        kSub_NotImplemented = 99, // stub: 未実装
    };
};

// =============================================================================
// CrashContext — クラッシュ 1 件分の context
// -----------------------------------------------------------------------------
// 全フィールドは Backend が「ReportCrash 呼び出し中のみ」参照する想定。
// 文字列ポインタは呼出側 (= クラッシュハンドラ) が寿命を保証する。stack_trace は
// 改行区切りの 1 本文字列 (Sentry/BugSnag が要求する正規化は backend 側で行う)。
// =============================================================================
struct CrashContext {
    const char* exception_type = nullptr;  // "SEH_ACCESS_VIOLATION" / "std::bad_alloc" 等
    const char* message        = nullptr;  // 人間可読の説明 ("null deref in Foo::Bar" 等)
    const char* stack_trace    = nullptr;  // 改行区切りスタック (空でも可)
    const char* scene_name     = nullptr;  // クラッシュ時にアクティブなシーン名 (任意)
    u64         frame_count    = 0;        // 起動から何フレーム経過した時か
    u64         timestamp      = 0;        // UNIX epoch ミリ秒 (任意; 0 = 未設定)
    const char* build_id       = nullptr;  // ビルド識別子 ("v1.2.3-abcdef" 等)
};

// =============================================================================
// ICrashReporterBackend — クラッシュ報告 backend の抽象 seam
// -----------------------------------------------------------------------------
// 1 タイトルにつき通常 1 インスタンス (Singleton 的運用)。
// 寿命はタイトル側 (acs::FApplication 等) が握る。
// =============================================================================
class ICrashReporterBackend {
public:
    ICrashReporterBackend() noexcept = default;
    virtual ~ICrashReporterBackend() noexcept = default;

    ICrashReporterBackend(const ICrashReporterBackend&)            = delete;
    ICrashReporterBackend& operator=(const ICrashReporterBackend&) = delete;
    ICrashReporterBackend(ICrashReporterBackend&&)                 = delete;
    ICrashReporterBackend& operator=(ICrashReporterBackend&&)      = delete;

    // SDK 初期化。`product_id` (例: "com.example.mygame") + `version`
    // (例: "1.2.3") はサービス上で集計キーになる。両方とも呼出側が寿命を
    // 保証する static / member バッファ。多重 Init の可否は実装依存。
    virtual TResult<void> Init(const char* product_id, const char* version) noexcept = 0;

    // 終了処理。Init() 前に呼んでも安全 (no-op)。残った breadcrumb / event は
    // 可能な範囲で flush することが望ましい (本 interface では強制しない)。
    virtual void Shutdown() noexcept = 0;

    // Init() 成功後かつ Shutdown() 前なら true。stub は常に false 寄り。
    virtual bool IsAvailable() const noexcept = 0;

    // クラッシュ 1 件を送信する。プロセスがまだ生きている前提 (uncatchable な
    // SEH/POSIX signal は具象実装の signal handler 側で別経路にする想定)。
    // `ctx` のフィールドは関数戻りまで生存していれば良い。
    virtual TResult<void> ReportCrash(const CrashContext& ctx) noexcept = 0;

    // 非致命的エラーを送信する。`category` は集計用のキー ("net" / "save" /
    // "shader" 等)、`message` は人間可読のメッセージ。
    virtual TResult<void> ReportError(const char* category, const char* message) noexcept = 0;

    // クラッシュ前の context をリングバッファに 1 件追加する。Sentry/BugSnag の
    // breadcrumb モデル。category / message のセマンティクスは ReportError と
    // 同じだが、こちらは送信せず、ReportCrash 時にまとめて添付される想定。
    virtual TResult<void> AddBreadcrumb(const char* category, const char* message) noexcept = 0;

    // 匿名ユーザー ID を設定する。GDPR / CCPA 対応のため、生 email / OS account
    // 名ではなく、初回起動時に生成した UUID 等を渡すこと。`anonymous_id` の
    // 寿命は呼出側が保証する (Backend がコピーするかどうかは実装依存)。
    virtual void SetUserId(const char* anonymous_id) noexcept = 0;

    // 非同期送信キューの pump。毎フレーム呼ばれる前提。`dt` は前フレームからの
    // 経過秒。タイムアウト判定や retry スケジューリングに使う。
    virtual void Tick(f32 dt) noexcept = 0;
};

// =============================================================================
// CrashReporterStub — ICrashReporterBackend の null-object 実装
// -----------------------------------------------------------------------------
// 全 API が NotImplemented を返す defensive stub。Init() ですら成功扱いに
// しないことで、本番ビルドに stub が紛れ込んだ場合に QA 工程で検出可能にする。
// (これは BackendClientStub と同じ方針。SteamworksBridgeStub は Init() のみ
// 成功扱いだったが、FCrashReporter はより厳格にしておく。)
// =============================================================================
class CrashReporterStub final : public ICrashReporterBackend {
public:
    CrashReporterStub() noexcept = default;
    ~CrashReporterStub() noexcept override = default;

    TResult<void> Init(const char* product_id, const char* version) noexcept override;
    void         Shutdown() noexcept override;
    bool         IsAvailable() const noexcept override { return false; }
    TResult<void> ReportCrash(const CrashContext& ctx) noexcept override;
    TResult<void> ReportError(const char* category, const char* message) noexcept override;
    TResult<void> AddBreadcrumb(const char* category, const char* message) noexcept override;
    void         SetUserId(const char* anonymous_id) noexcept override;
    void         Tick(f32 dt) noexcept override;
};

// プロセス共有の stub ICrashReporterBackend。常に NotImplemented を返す。
// 本体側 (タイトル / サンプル) はまずこれを使ってリンクを通す。具象実装に
// 切り替える際は `m_Crash` メンバ等に CrashReporterSentry 等を差し替える。
ICrashReporterBackend& GetCrashStub() noexcept;

// =============================================================================
// CrashHandler — 高レベル wrapper
// -----------------------------------------------------------------------------
// ICrashReporterBackend を 1 つ抱えて、タイトル側のホットパスで使いやすい
// API を提供する thin wrapper。
//   ・Install(backend) で参照を保持し、Uninstall() で外す。
//   ・NotifyCrash() は CrashContext を最小フィールドだけ埋めて ReportCrash を
//     呼ぶ簡略パス。
//   ・AddBreadcrumb() は backend に素通し。
// backend == nullptr の状態 (Install 前 / Uninstall 後) では全 API が no-op
// になる (= 二次クラッシュ防止)。
// =============================================================================
class CrashHandler {
public:
    CrashHandler() noexcept = default;
    ~CrashHandler() noexcept = default;

    CrashHandler(const CrashHandler&)            = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;
    CrashHandler(CrashHandler&&)                 = delete;
    CrashHandler& operator=(CrashHandler&&)      = delete;

    // backend は呼出側が所有 (Install は寿命を借りるだけ)。
    // nullptr を渡すと Uninstall と同義。
    void Install(ICrashReporterBackend* backend) noexcept;

    // backend 参照を外す。多重呼出 / 未 Install 呼出は no-op (べき等)。
    void Uninstall() noexcept;

    // 簡略クラッシュ通知。frame_count / timestamp 等の追加 context は
    // 後で別 API を生やすときに拡張する。Backend が nullptr の場合は no-op。
    void NotifyCrash(const char* exception_type, const char* message) noexcept;

    // Backend に素通し。Backend が nullptr の場合は no-op。
    void AddBreadcrumb(const char* category, const char* message) noexcept;

private:
    ICrashReporterBackend* m_Backend = nullptr;
};

} // namespace acs::game
