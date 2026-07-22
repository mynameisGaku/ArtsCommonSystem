// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CrashReporter (ship build 専用クラッシュ報告 seam)
//
// 役割:
//   出荷ビルドでプロセスが落ちた時、外部のクラッシュ集約サービス (Sentry /
//   Crashpad / Backtrace.io / BugSnag 等) へ最低限の context を吐き出すための
//   **抽象 seam**。ACS 本体は具象な HTTP/IPC スタックを抱え込まず、
//   `ICrashReporterBackend` インターフェイスと NotImplemented を返すだけの
//   `FCrashReporterStub` のみを提供する。
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
// 範囲外:
//   ・OS signal / SEH ハンドラの登録          (具象実装側の責務)
//   ・minidump / coredump の収集とアップロード (具象実装側の責務)
//   ・PII (個人情報) のフィルタ                (呼出側が breadcrumb に詰める段で守る)
//   ・symbolication (シンボル解決)            (サーバ側 / build pipeline の責務)
//
// 設計選択:
//   ・**stub interface のみ**: 本ヘッダ + .cpp は ICrashReporterBackend を
//     **抽象 interface として宣言** し、合わせて **常に NotImplemented を返す
//     FCrashReporterStub** を提供するだけ。ACS 本体がリンク時に「最低 1 実装が
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

/**
 * クラッシュ報告 backend の FErrorCode サブコード定義。
 *
 * @details EErrCategory は Generic を使い、stub は kSub_NotImplemented を返す。
 */
struct FCrashReporterError {
    /** クラッシュ報告 API が返すエラーサブコード。 */
    enum ESubCode : u16 {
        /** Init() 前に API を呼んだ。 */
        kSub_NotInitialized = 1,

        /** 2 重 Init (許容するかは実装依存)。 */
        kSub_AlreadyInited  = 2,

        /** product_id / message が nullptr 等の不正引数。 */
        kSub_BadArgument    = 3,

        /** 内部キュー溢れ (breadcrumb / event)。 */
        kSub_QueueFull      = 4,

        /** アップロード失敗。 */
        kSub_NetworkFailure = 5,

        /** stub: 未実装。 */
        kSub_NotImplemented = 99,
    };
};

/**
 * クラッシュ 1 件分の context。
 *
 * @details
 * 全フィールドは Backend が ReportCrash 呼び出し中のみ参照する想定。文字列ポインタは
 * 呼出側 (クラッシュハンドラ) が寿命を保証する非所有ポインタ。
 */
struct FCrashContext {
    /** 例外/シグナルの種別 ("SEH_ACCESS_VIOLATION" / "std::bad_alloc" 等)。 */
    const char* exception_type = nullptr;

    /** 人間可読の説明 ("null deref in Foo::Bar" 等)。 */
    const char* message        = nullptr;

    /** 改行区切りのスタックトレース 1 本文字列 (空でも可)。 */
    const char* stack_trace    = nullptr;

    /** クラッシュ時にアクティブなシーン名 (任意)。 */
    const char* scene_name     = nullptr;

    /** 起動から何フレーム経過した時点か。 */
    u64         frame_count    = 0;

    /** UNIX epoch ミリ秒 (任意; 0 = 未設定)。 */
    u64         timestamp      = 0;

    /** ビルド識別子 ("v1.2.3-abcdef" 等)。 */
    const char* build_id       = nullptr;
};

/**
 * クラッシュ報告 backend の抽象 seam。
 *
 * @details
 * Sentry / Crashpad / Backtrace.io / BugSnag 等の具象実装を差し込むためのインターフェイス。
 * 1 タイトルにつき通常 1 インスタンス (Singleton 的運用) で、寿命はタイトル側
 * (acs::FApplication 等) が握る。全 API は二次クラッシュ防止のため noexcept。
 */
class ICrashReporterBackend {
public:
    /** 抽象基底を構築する。 */
    ICrashReporterBackend() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~ICrashReporterBackend() noexcept = default;

    /** コピー禁止 (backend は単一インスタンスで運用するため)。 */
    ICrashReporterBackend(const ICrashReporterBackend&)            = delete;

    /** コピー代入も禁止。 */
    ICrashReporterBackend& operator=(const ICrashReporterBackend&) = delete;

    /** ムーブ禁止。 */
    ICrashReporterBackend(ICrashReporterBackend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ICrashReporterBackend& operator=(ICrashReporterBackend&&)      = delete;

    /**
     * SDK を初期化する。
     *
     * @details product_id / version はサービス上の集計キー。両方とも呼出側が寿命を保証する
     * static / member バッファ。多重 Init の可否は実装依存。
     * @param product_id 製品識別子 (例: "com.example.mygame")。
     * @param version バージョン文字列 (例: "1.2.3")。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Init(const char* product_id, const char* version) noexcept = 0;

    /**
     * 終了処理を行う。
     *
     * @details Init() 前に呼んでも安全 (no-op)。残った breadcrumb / event は可能な範囲で
     * flush することが望ましい (本 interface では強制しない)。
     */
    virtual void Shutdown() noexcept = 0;

    /**
     * backend が利用可能かを返す。
     *
     * @return Init() 成功後かつ Shutdown() 前なら true。
     */
    virtual bool IsAvailable() const noexcept = 0;

    /**
     * クラッシュ 1 件を送信する。
     *
     * @details プロセスがまだ生きている前提 (uncatchable な SEH/POSIX signal は具象実装の
     * signal handler 側で別経路にする想定)。ctx のフィールドは関数戻りまで生存していれば良い。
     * @param ctx 送信するクラッシュ context。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> ReportCrash(const FCrashContext& ctx) noexcept = 0;

    /**
     * 非致命的エラーを送信する。
     *
     * @param category 集計用のキー ("net" / "save" / "shader" 等)。
     * @param message 人間可読のメッセージ。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> ReportError(const char* category, const char* message) noexcept = 0;

    /**
     * クラッシュ前の context をリングバッファに 1 件追加する (breadcrumb モデル)。
     *
     * @details ReportError と引数の意味は同じだが、こちらは即時送信せず ReportCrash 時に
     * まとめて添付される想定。
     * @param category 集計用のキー。
     * @param message 人間可読のメッセージ。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> AddBreadcrumb(const char* category, const char* message) noexcept = 0;

    /**
     * 匿名ユーザー ID を設定する。
     *
     * @details GDPR / CCPA 対応のため、生 email / OS account 名ではなく初回起動時に生成した
     * UUID 等を渡すこと。anonymous_id の寿命は呼出側が保証する (コピーするかは実装依存)。
     * @param anonymous_id 設定する匿名 ID 文字列。
     */
    virtual void SetUserId(const char* anonymous_id) noexcept = 0;

    /**
     * 非同期送信キューを pump する (毎フレーム呼ばれる前提)。
     *
     * @details タイムアウト判定や retry スケジューリングに使う。
     * @param dt 前フレームからの経過秒。
     */
    virtual void Tick(f32 dt) noexcept = 0;
};

/**
 * ICrashReporterBackend の null-object 実装。
 *
 * @details
 * 全 API が NotImplemented を返す defensive stub。Init() ですら成功扱いにしないことで、
 * 本番ビルドに stub が紛れ込んだ場合に QA 工程で検出可能にする。
 */
class FCrashReporterStub final : public ICrashReporterBackend {
public:
    /** stub を構築する。 */
    FCrashReporterStub() noexcept = default;

    /** 破棄する。 */
    ~FCrashReporterStub() noexcept override = default;

    /**
     * 常に NotImplemented エラーを返す (初期化しない)。
     *
     * @param product_id 無視される製品識別子。
     * @param version 無視されるバージョン文字列。
     * @return kSub_NotImplemented のエラー。
     */
    TResult<void> Init(const char* product_id, const char* version) noexcept override;

    /** no-op (stub は never-initialized 状態のため何もしない)。 */
    void         Shutdown() noexcept override;

    /**
     * 常に利用不可を返す。
     *
     * @return 常に false。
     */
    bool         IsAvailable() const noexcept override { return false; }

    /**
     * 常に NotImplemented エラーを返す (送信しない)。
     *
     * @param ctx 無視されるクラッシュ context。
     * @return kSub_NotImplemented のエラー。
     */
    TResult<void> ReportCrash(const FCrashContext& ctx) noexcept override;

    /**
     * 常に NotImplemented エラーを返す (送信しない)。
     *
     * @param category 無視されるカテゴリ。
     * @param message 無視されるメッセージ。
     * @return kSub_NotImplemented のエラー。
     */
    TResult<void> ReportError(const char* category, const char* message) noexcept override;

    /**
     * 常に NotImplemented エラーを返す (追加しない)。
     *
     * @param category 無視されるカテゴリ。
     * @param message 無視されるメッセージ。
     * @return kSub_NotImplemented のエラー。
     */
    TResult<void> AddBreadcrumb(const char* category, const char* message) noexcept override;

    /**
     * no-op (stub はユーザー ID を保持しない)。
     *
     * @param anonymous_id 無視される匿名 ID。
     */
    void         SetUserId(const char* anonymous_id) noexcept override;

    /**
     * no-op (stub には pump 対象がない)。
     *
     * @param dt 無視される経過秒。
     */
    void         Tick(f32 dt) noexcept override;
};

/**
 * プロセス共有の stub backend を返す。
 *
 * @details 常に NotImplemented を返す process-wide singleton。本体側 (タイトル / サンプル)
 * はまずこれを使ってリンクを通し、後から具象実装に差し替える。
 * @return stub ICrashReporterBackend への参照。
 */
ICrashReporterBackend& GetCrashStub() noexcept;

/**
 * 既定 backend を返す provider 関数の型。
 *
 * @details
 * gameframework は実 backend モジュール (循環依存になる) に依存できないため、実 backend 側が
 * この型の関数を SetCrashReporterProvider で登録し、ゲームコードは GetDefaultCrashReporter
 * 経由で backend 非依存に既定 backend を取得する。
 */
using CrashReporterProvider = ICrashReporterBackend& (*)() noexcept;

/**
 * 既定 backend provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details nullptr を登録すると stub に戻す。後勝ち。
 * @param provider 既定 backend を返す関数 (nullptr で解除)。
 */
void SetCrashReporterProvider(CrashReporterProvider provider) noexcept;

/**
 * 既定 backend を返す。
 *
 * @return provider 登録済みならその実 backend、未登録なら GetCrashStub()。
 */
ICrashReporterBackend& GetDefaultCrashReporter() noexcept;

/**
 * ICrashReporterBackend を 1 つ抱える高レベル thin wrapper。
 *
 * @details
 * Install(backend) で参照を借り、Uninstall() で外す。NotifyCrash() は FCrashContext の最小
 * フィールドだけ埋めて ReportCrash を呼ぶ簡略パス、AddBreadcrumb() は backend に素通しする。
 * backend == nullptr の状態 (Install 前 / Uninstall 後) では全 API が no-op になる
 * (二次クラッシュ防止)。
 */
class FCrashHandler {
public:
    /** backend 未設定状態で構築する。 */
    FCrashHandler() noexcept = default;

    /** 破棄する (backend の所有権は持たないため解放しない)。 */
    ~FCrashHandler() noexcept = default;

    /** コピー禁止。 */
    FCrashHandler(const FCrashHandler&)            = delete;

    /** コピー代入も禁止。 */
    FCrashHandler& operator=(const FCrashHandler&) = delete;

    /** ムーブ禁止。 */
    FCrashHandler(FCrashHandler&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FCrashHandler& operator=(FCrashHandler&&)      = delete;

    /**
     * 使用する backend を設定する (寿命を借りるだけ)。
     *
     * @details 多重 Install は最後の 1 個で上書き。nullptr を渡すと Uninstall と同義。
     * @param backend 呼出側が所有する backend (nullptr 可)。
     */
    void Install(ICrashReporterBackend* backend) noexcept;

    /** backend 参照を外す。多重呼出 / 未 Install 呼出は no-op (べき等)。 */
    void Uninstall() noexcept;

    /**
     * 簡略クラッシュ通知 (最小フィールドだけ埋めて ReportCrash を呼ぶ)。
     *
     * @details backend が nullptr の場合は no-op。
     * @param exception_type 例外/シグナルの種別文字列。
     * @param message 人間可読の説明。
     */
    void NotifyCrash(const char* exception_type, const char* message) noexcept;

    /**
     * breadcrumb を backend に素通しで追加する。
     *
     * @details backend が nullptr の場合は no-op。
     * @param category 集計用のキー。
     * @param message 人間可読のメッセージ。
     */
    void AddBreadcrumb(const char* category, const char* message) noexcept;

private:
    /** 借用中の backend (非所有。未設定なら nullptr)。 */
    ICrashReporterBackend* m_Backend = nullptr;
};

} // namespace acs::game
