// SPDX-License-Identifier: Apache-2.0
// Windows DbgHelp minidump backend for acs::game::ICrashReporterBackend.
#pragma once

#include "gameframework/CrashReporter.h"

namespace acs::crashwin {

/**
 * CWindowsCrashReporter の生成時設定。
 *
 * @details コンストラクタ経由で minidump / テキストレポートの出力先ディレクトリを指定する。
 */
struct FWindowsCrashReporterConfig {
    /** ダンプとレポートの出力先ディレクトリ (既定 "crash_dumps")。 */
    const char* DumpDirectory = "crash_dumps";
};

/**
 * Windows DbgHelp を使う ICrashReporterBackend の実装。
 *
 * @details
 * クラッシュ / 非致命的エラー発生時に、ダンプディレクトリ配下へテキストレポート (.txt) と
 * MiniDumpWriteDump によるプロセス minidump (.dmp) を書き出す自己完結型の backend。
 * breadcrumb は固定長リングバッファで保持し、レポート出力時にまとめて添付する。文字列は
 * 全て固定長メンババッファへコピーして保持する (非所有ポインタに依存しない)。
 */
class CWindowsCrashReporter final : public acs::game::ICrashReporterBackend {
public:
    /** 既定ダンプディレクトリ "crash_dumps" で構築する。 */
    CWindowsCrashReporter() noexcept = default;

    /**
     * 設定からダンプディレクトリを取り込んで構築する。
     *
     * @param config 出力先ディレクトリ等の生成時設定。
     */
    explicit CWindowsCrashReporter(const FWindowsCrashReporterConfig& config) noexcept;

    /** 破棄する (保持リソースは固定長バッファのみのため特別な解放は不要)。 */
    ~CWindowsCrashReporter() noexcept override = default;

    /** コピー禁止 (プロセス共有 singleton として単独運用するため)。 */
    CWindowsCrashReporter(const CWindowsCrashReporter&)            = delete;

    /** コピー代入も禁止。 */
    CWindowsCrashReporter& operator=(const CWindowsCrashReporter&) = delete;

    /** ムーブ禁止。 */
    CWindowsCrashReporter(CWindowsCrashReporter&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CWindowsCrashReporter& operator=(CWindowsCrashReporter&&)      = delete;

    /**
     * backend を初期化し、ダンプディレクトリを作成する。
     *
     * @details
     * product_id / version を内部バッファへコピーし、ダンプディレクトリを CreateDirectoryA
     * で作成する (既存なら成功扱い)。成功後 m_bInitialized を立てる。
     * @param product_id サービス集計キーになる製品 ID (空文字は不可)。
     * @param version 同じく集計キーになるバージョン文字列 (空文字は不可)。
     * @return 成功なら空の TResult、引数不正やディレクトリ作成失敗ならエラー。
     */
    acs::TResult<void> Init(const char* product_id, const char* version) noexcept override;

    /** backend を終了状態にする (m_bInitialized を下ろすだけ)。 */
    void               Shutdown() noexcept override;

    /**
     * backend が利用可能 (Init 済み) かを返す。
     *
     * @return Init() 成功後かつ Shutdown() 前なら true。
     */
    bool               IsAvailable() const noexcept override;

    /**
     * クラッシュ 1 件のテキストレポートと minidump を書き出す。
     *
     * @details
     * crash_<日時>_pid<PID>_<連番> をベースに .txt レポートと .dmp ダンプを生成し、
     * それぞれのパスを m_LastReportPath / m_LastDumpPath へ記録する。
     * @param context クラッシュ 1 件分の context (例外種別・メッセージ・スタック等)。
     * @return 成功なら空の TResult、未初期化やファイル書き込み失敗ならエラー。
     */
    acs::TResult<void> ReportCrash(const acs::game::FCrashContext& context) noexcept override;

    /**
     * 非致命的エラー 1 件のテキストレポートを書き出す (minidump は作らない)。
     *
     * @details error_<日時>_... をベースに .txt レポートのみ生成し、m_LastDumpPath は空にする。
     * @param category 集計用のカテゴリキー。
     * @param message 人間可読のエラーメッセージ。
     * @return 成功なら空の TResult、未初期化や引数 nullptr、書き込み失敗ならエラー。
     */
    acs::TResult<void> ReportError(const char* category, const char* message) noexcept override;

    /**
     * breadcrumb を 1 件リングバッファへ追加する。
     *
     * @details
     * 満杯時は最古の 1 件を上書きする (FIFO)。次回のレポート出力時にまとめて添付される。
     * @param category breadcrumb のカテゴリ (最大 31 文字でコピー)。
     * @param message breadcrumb のメッセージ (最大 159 文字でコピー)。
     * @return 成功なら空の TResult、未初期化や引数 nullptr ならエラー。
     */
    acs::TResult<void> AddBreadcrumb(const char* category, const char* message) noexcept override;

    /**
     * レポートに付与する匿名ユーザー ID を設定する。
     *
     * @param anonymous_id 匿名ユーザー ID (内部バッファへコピー、nullptr は空扱い)。
     */
    void               SetUserId(const char* anonymous_id) noexcept override;

    /**
     * 毎フレーム pump フック (本 backend は同期書き込みのため何もしない)。
     *
     * @param dt 前フレームからの経過秒 (未使用)。
     */
    void               Tick(acs::f32 dt) noexcept override;

    /**
     * 直近に書き出した minidump のパスを返す。
     *
     * @return 最後の .dmp パス (未生成や ReportError 直後なら空文字)。
     */
    const char* LastDumpPath() const noexcept { return m_LastDumpPath; }

    /**
     * 直近に書き出したテキストレポートのパスを返す。
     *
     * @return 最後の .txt パス (未生成なら空文字)。
     */
    const char* LastReportPath() const noexcept { return m_LastReportPath; }

private:
    /**
     * breadcrumb リングバッファの 1 エントリ。
     *
     * @details category / message を固定長バッファに保持する。
     */
    struct FBreadcrumb {
        /** breadcrumb のカテゴリ (NUL 終端、最大 31 文字)。 */
        char Category[32] = {};

        /** breadcrumb のメッセージ (NUL 終端、最大 159 文字)。 */
        char Message[160] = {};
    };

    /** breadcrumb リングバッファの最大件数。 */
    static constexpr acs::u32 kMaxBreadcrumbs = 32;

    /** I/O 失敗 (ディレクトリ作成・ファイル書き込み) を表すエラーサブコード。 */
    static constexpr acs::u16 kSubCrashWinIoFailed = 2001;

    /** minidump 書き込み失敗を表すエラーサブコード。 */
    static constexpr acs::u16 kSubCrashWinDumpFailed = 2002;

    /**
     * src を dst へ安全に NUL 終端コピーする (バッファ境界を超えない)。
     *
     * @param dst コピー先バッファ。
     * @param dst_size dst のバイト数 (NUL 終端領域込み)。
     * @param src コピー元文字列 (nullptr なら dst を空文字にする)。
     */
    void CopyText(char* dst, acs::usize dst_size, const char* src) noexcept;

    /**
     * レポート用の一意なベースパス (拡張子なし) を組み立てる。
     *
     * @details
     * "ダンプディレクトリ\prefix_<日時>_pid<PID>_<連番>" の形式。連番は m_ReportSerial を
     * インクリメントして付与し、同一秒内の衝突を避ける。
     * @param out ベースパスを書き込む先のバッファ。
     * @param out_size out のバイト数。
     * @param prefix ファイル名の接頭辞 ("crash" / "error" 等)。
     */
    void BuildBasePath(char* out, acs::usize out_size, const char* prefix) noexcept;

    /**
     * テキスト形式のクラッシュ / エラーレポートをファイルへ書き出す。
     *
     * @details
     * product/version/user に加え、context があれば例外種別・スタック等を、breadcrumb は
     * 全件を key=value 形式で出力する。
     * @param path 書き込み先のファイルパス。
     * @param context クラッシュ context (エラーレポート時は nullptr)。
     * @param category エラーカテゴリ (クラッシュレポート時は nullptr)。
     * @param message エラーメッセージ (クラッシュレポート時は nullptr)。
     * @return 書き込みに成功したら true、ファイルを開けなければ false。
     */
    bool WriteTextReport(const char* path,
                         const acs::game::FCrashContext* context,
                         const char* category,
                         const char* message) noexcept;

    /**
     * 現在のプロセスの minidump を MiniDumpWriteDump で書き出す。
     *
     * @param path 書き込み先の .dmp ファイルパス。
     * @return 書き込みに成功したら true、ファイル作成や MiniDumpWriteDump 失敗なら false。
     */
    bool WriteCurrentProcessDump(const char* path) noexcept;

    /** ダンプとレポートの出力先ディレクトリ。 */
    char m_DumpDirectory[260] = "crash_dumps";

    /** Init で受け取った製品 ID。 */
    char m_ProductId[96] = {};

    /** Init で受け取ったバージョン文字列。 */
    char m_Version[48] = {};

    /** レポートに付与する匿名ユーザー ID (未設定なら空)。 */
    char m_UserId[96] = {};

    /** 直近に書き出した minidump のパス。 */
    char m_LastDumpPath[260] = {};

    /** 直近に書き出したテキストレポートのパス。 */
    char m_LastReportPath[260] = {};

    /** breadcrumb リングバッファ本体。 */
    FBreadcrumb m_Breadcrumbs[kMaxBreadcrumbs] = {};

    /** リングバッファ内の最古エントリのインデックス。 */
    acs::u32    m_BreadcrumbStart = 0;

    /** 現在保持している breadcrumb の件数 (最大 kMaxBreadcrumbs)。 */
    acs::u32    m_BreadcrumbCount = 0;

    /** レポートファイル名に付ける連番 (生成のたびにインクリメント)。 */
    acs::u32    m_ReportSerial = 0;

    /** Init 済みフラグ。 */
    bool        m_bInitialized = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWindowsCrashReporter = CWindowsCrashReporter;

/**
 * プロセス共有の既定 CWindowsCrashReporter singleton を返す (provider の実体)。
 *
 * @details
 * 初回アクセス時に ACS 既定のプレースホルダ product id / version で Init() を 1 回走らせ、
 * すぐ使える状態の Windows DbgHelp backend を返す。
 * @return プロセス共有の既定 backend への参照。
 */
acs::game::ICrashReporterBackend& GetDefaultWindowsCrashReporter() noexcept;

/**
 * gameframework の CrashReporter provider に本 backend を登録する。
 *
 * @details
 * 以降 acs::game::GetDefaultCrashReporter() が GetDefaultWindowsCrashReporter() の実 backend
 * を返すようになる。アプリ起動時に一度だけ呼ぶ。
 */
void InstallWindowsCrashReporterAsDefault() noexcept;

} // namespace acs::crashwin
