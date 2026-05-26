// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CrashReporter stub 実装
//
// 本ファイルは CrashReporter.h で宣言した ICrashReporterBackend に対し、
// 「常に NotImplemented を返すだけ」の defensive stub (`CrashReporterStub`) と、
// 高レベル wrapper (`CrashHandler`) を提供する。
//
// 目的:
//   ・ACS 本体 / サンプルが Sentry / Crashpad 等の実装の有無に関わらず
//     リンクを通せるようにする (Pillar O の seam 要件)。
//   ・タイトル側が `ICrashReporterBackend* p = &acs::game::GetCrashStub();` の
//     ように null-object パターンで保持し、後から具象実装に差し替える経路を
//     確保する。
//   ・stub に対する Init / ReportCrash 呼び出しは「成功扱いで黙る」のではなく
//     **必ず TResult<...> Err を返す**ことで、本番ビルドに stub が紛れ込んだ
//     ケースを QA 工程で検出可能にしておく。
//
// 将来 (Phase 2 以降, Pillar O 本実装フェーズ):
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
//   ・CrashHandler の `_backend == nullptr` チェックは「二次クラッシュ防止」が
//     主目的。クラッシュ報告 API 自体で SEGV を起こすと最悪なので、guard を
//     最優先に入れる。

#include "gameframework/CrashReporter.h"

#include "foundation/Error.h"

namespace acs::game {

// -----------------------------------------------------------------------------
// CrashReporterStub — ICrashReporterBackend の null-object 実装
// -----------------------------------------------------------------------------

TResult<void> CrashReporterStub::Init(const char* product_id, const char* version) noexcept {
    (void)product_id;
    (void)version;
    return ACS_ERR(Generic, CrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::Init is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

void CrashReporterStub::Shutdown() noexcept {
    // stub は never-initialized 状態。no-op で安全に通す。
}

TResult<void> CrashReporterStub::ReportCrash(const CrashContext& ctx) noexcept {
    (void)ctx;
    return ACS_ERR(Generic, CrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::ReportCrash is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

TResult<void> CrashReporterStub::ReportError(const char* category, const char* message) noexcept {
    (void)category;
    (void)message;
    return ACS_ERR(Generic, CrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::ReportError is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

TResult<void> CrashReporterStub::AddBreadcrumb(const char* category, const char* message) noexcept {
    (void)category;
    (void)message;
    return ACS_ERR(Generic, CrashReporterError::kSub_NotImplemented,
                   "ICrashReporterBackend::AddBreadcrumb is not implemented "
                   "(stub: link a concrete crash reporter implementation)");
}

void CrashReporterStub::SetUserId(const char* anonymous_id) noexcept {
    (void)anonymous_id;
    // stub はユーザー ID を保持しない。no-op。
}

void CrashReporterStub::Tick(f32 dt) noexcept {
    (void)dt;
    // stub には pump 対象なし。no-op。
}

// -----------------------------------------------------------------------------
// アクセサ: function-local static で process-wide singleton を保持
// -----------------------------------------------------------------------------
// C++11 magic statics により初期化は thread-safe。
// 破棄順序は他の static と独立で良い (相互依存なし)。
ICrashReporterBackend& GetCrashStub() noexcept {
    static CrashReporterStub s_instance;
    return s_instance;
}

// -----------------------------------------------------------------------------
// CrashHandler — 高レベル wrapper
// -----------------------------------------------------------------------------
// `_backend == nullptr` チェックを全 API に入れて二次クラッシュを防ぐ。
// クラッシュ報告系のホットパスでは branch 1 本の方が安全で、性能ペナルティも
// 無視できる (どうせ呼ばれる頻度は低い)。

void CrashHandler::Install(ICrashReporterBackend* backend) noexcept {
    // nullptr 受け入れは Uninstall と同義。多重 Install は最後の 1 個で上書き。
    _backend = backend;
}

void CrashHandler::Uninstall() noexcept {
    _backend = nullptr;
}

void CrashHandler::NotifyCrash(const char* exception_type, const char* message) noexcept {
    if (_backend == nullptr) {
        // 未 Install: 黙って no-op。クラッシュ報告経路で SEGV を起こさない。
        return;
    }
    CrashContext ctx{};
    ctx.exception_type = exception_type;
    ctx.message        = message;
    // frame_count / timestamp / scene_name / build_id は呼出側が後から
    // 拡張 API で詰める想定。stub には届かないので TResult も無視で良い。
    (void)_backend->ReportCrash(ctx);
}

void CrashHandler::AddBreadcrumb(const char* category, const char* message) noexcept {
    if (_backend == nullptr) {
        return;
    }
    // backend が NotImplemented を返してもクラッシュ報告経路は止めない。
    (void)_backend->AddBreadcrumb(category, message);
}

} // namespace acs::game
