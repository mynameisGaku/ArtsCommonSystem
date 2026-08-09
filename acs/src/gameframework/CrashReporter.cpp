// SPDX-License-Identifier: Apache-2.0
// GameFramework crash reporter stub 実装
//
// 本ファイルは FCrashReporter.h で宣言した ICrashReporterBackend に対し、
// 「常に NotImplemented を返すだけ」の defensive stub (`CrashReporterStub`) と、
// 高レベル wrapper (`CrashHandler`) を提供する。
//
// 目的:
//   ・ACS の利用側が crash reporter 実装の有無に関わらず
//     リンクを通せるようにする (crash reporter seam の要件)。
//   ・タイトル側が `ICrashReporterBackend* p = &acs::game::GetCrashStub();` の
//     ように null-object パターンで保持し、後から具象実装に差し替える経路を
//     確保する。
//   ・stub に対する Init / ReportCrash 呼び出しは「成功扱いで黙る」のではなく
//     **必ず TResult<...> Err を返す**ことで、本番ビルドに stub が紛れ込んだ
//     ケースを QA 工程で検出可能にしておく。
//
// 具象実装側の責務:
//   ・Sentry Native SDK ラッパ (CrashReporterSentry)
//   ・Crashpad out-of-process minidump 連携 (CrashReporterCrashpad)
//   ・SEH / POSIX signal handler の登録と coredump pipe
//   ・PII フィルタ + breadcrumb リングバッファの具象実装
//
// 設計メモ:
//   ・Stub は **process-wide singleton** で十分。`static` ローカル変数で
//     thread-safe initialization (C++11 magic statics) を活用する。
//   ・コピー/ムーブは ICrashReporterBackend 側で delete 済みなので、stub 派生
//     クラスも自然に non-copy / non-movable。
//   ・全関数 noexcept。stub なので分岐も最小限。
//   ・引数バリデーション (nullptr 等) は本実装ではしない: NotImplemented を
//     先に返してしまうため。具象実装側で kSub_BadArgument を返す責務になる。
//   ・CrashHandler の `m_Backend == nullptr` チェックは「二次クラッシュ防止」が
//     主目的。クラッシュ報告 API 自体で SEGV を起こすと最悪なので、guard を
//     最優先に入れる。

#include "gameframework/CrashReporter.h"

#include "foundation/Error.h"
#include "threading/Atomic.h"

namespace acs::game {

/** stub: 初期化せず NotImplemented エラーを返す。 */
TResult<void> CCrashReporterStub::Init(const char* product_id, const char* version) noexcept {
    (void)product_id;
    (void)version;
    return ACS_ERR(Generic, FCrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::Init is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

/** stub: never-initialized 状態なので no-op。 */
void CCrashReporterStub::Shutdown() noexcept {
    // stub は never-initialized 状態。no-op で安全に通す。
}

/** stub: 送信せず NotImplemented エラーを返す。 */
TResult<void> CCrashReporterStub::ReportCrash(const FCrashContext& ctx) noexcept {
    (void)ctx;
    return ACS_ERR(Generic, FCrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::ReportCrash is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

/** stub: 送信せず NotImplemented エラーを返す。 */
TResult<void> CCrashReporterStub::ReportError(const char* category, const char* message) noexcept {
    (void)category;
    (void)message;
    return ACS_ERR(Generic, FCrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::ReportError is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

/** stub: 追加せず NotImplemented エラーを返す。 */
TResult<void> CCrashReporterStub::AddBreadcrumb(const char* category, const char* message) noexcept {
    (void)category;
    (void)message;
    return ACS_ERR(Generic, FCrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::AddBreadcrumb is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

/** stub: ユーザー ID を保持しないので no-op。 */
void CCrashReporterStub::SetUserId(const char* anonymous_id) noexcept {
    (void)anonymous_id;
    // stub はユーザー ID を保持しない。no-op。
}

/** stub: pump 対象がないので no-op。 */
void CCrashReporterStub::Tick(f32 dt) noexcept {
    (void)dt;
    // stub には pump 対象なし。no-op。
}

/** function-local static で process-wide な stub singleton を返す (magic statics)。 */
ICrashReporterBackend& GetCrashStub() noexcept {
    static CCrashReporterStub s_instance;
    return s_instance;
}

namespace {
/** 登録済みの既定 backend provider (未登録なら nullptr)。 */
TAtomic<CrashReporterProvider> g_CrashReporterProvider{nullptr};
}

/** 既定 backend provider を差し替える (後勝ち)。 */
void SetCrashReporterProvider(CrashReporterProvider Provider) noexcept
{
    g_CrashReporterProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みなら実 backend、未登録なら stub を返す。 */
ICrashReporterBackend& GetDefaultCrashReporter() noexcept
{
    const CrashReporterProvider Provider = g_CrashReporterProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetCrashStub();
}

/** 借用 backend を設定する (nullptr は Uninstall と同義、後勝ち)。 */
void CCrashHandler::Install(ICrashReporterBackend* backend) noexcept {
    // nullptr 受け入れは Uninstall と同義。多重 Install は最後の 1 個で上書き。
    m_Backend = backend;
}

/** 借用 backend 参照を外す (べき等)。 */
void CCrashHandler::Uninstall() noexcept {
    m_Backend = nullptr;
}

/** backend があれば最小 context で ReportCrash を呼ぶ (nullptr なら no-op)。 */
void CCrashHandler::NotifyCrash(const char* exception_type, const char* message) noexcept {
    if (m_Backend == nullptr) {
        // 未 Install: 黙って no-op。クラッシュ報告経路で SEGV を起こさない。
        return;
    }
    FCrashContext ctx{};
    ctx.exception_type = exception_type;
    ctx.message        = message;
    // frame_count / timestamp / scene_name / build_id は呼出側が後から
    // 拡張 API で詰める想定。stub には届かないので TResult も無視で良い。
    (void)m_Backend->ReportCrash(ctx);
}

/** backend があれば breadcrumb を素通しする (nullptr なら no-op)。 */
void CCrashHandler::AddBreadcrumb(const char* category, const char* message) noexcept {
    if (m_Backend == nullptr) {
        return;
    }
    // backend が NotImplemented を返してもクラッシュ報告経路は止めない。
    (void)m_Backend->AddBreadcrumb(category, message);
}

} // namespace acs::game
