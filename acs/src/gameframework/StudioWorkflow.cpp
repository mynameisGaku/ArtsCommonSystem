// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar W — StudioWorkflow stub 実装 (FAssetLockingStub / FBuildFarmStub)
//
// 本ファイルは StudioWorkflow.h で宣言した 2 つの interface に対し、
// 「常に NotImplemented を返すだけ」の defensive stub を提供する。
//
// 目的:
//   ・ACS 本体 / エディタが Perforce / Plastic / Jenkins SDK の有無に関わらず
//     リンクを通せるようにする (Pillar W の seam 要件)。
//   ・タイトル側 / エディタ側が
//       `IAssetLockingBackend* p = &acs::game::GetAssetLockingStub();`
//     のように null-object パターンで保持し、後から具象実装に差し替える経路を
//     確保する。
//   ・stub に対する LockAsset / SubmitBuild 等の呼び出しは「成功扱いで黙る」ではなく
//     **必ず TResult<...> Err を返す** ことで、本番ビルドに stub が紛れ込んだ
//     ケースを QA 工程で検出可能にしておく。
//
// 将来 (Phase 2 以降, StudioWorkflow モジュール本実装フェーズ):
//   ・PerforceAssetLocking         — libp4api 経由の P4 ロック (主流)
//   ・PlasticAssetLocking          — Plastic SCM の Smart Locks 連携
//   ・GitLfsAssetLocking           — Git LFS file locking API (中規模スタジオ向け)
//   ・JenkinsBuildFarm             — Jenkins REST API + multibranch pipeline 連携
//   ・TeamCityBuildFarm            — TeamCity REST API 連携
//   ・GithubActionsBuildFarm       — GitHub Actions workflow dispatch + Artifacts
//   ・UnityCloudBuildFarm          — Unity Cloud Build (Unity プロジェクト時のみ)
//
// 設計メモ:
//   ・Stub は **process-wide singleton** で十分。`static` ローカル変数で
//     thread-safe initialization (C++11 magic statics) を活用する。
//   ・コピー/ムーブは IAssetLockingBackend / IBuildFarmBackend 側で delete 済み
//     なので、stub 派生クラスも自然に non-copy / non-movable。
//   ・全関数 noexcept。stub なので分岐も最小限。
//   ・引数バリデーション (nullptr 等) は本実装ではしない: NotImplemented を
//     先に返してしまうため。具象実装側で kSub_BadArgument を返す責務になる。
#include "gameframework/StudioWorkflow.h"

#include "foundation/Error.h"

namespace acs::game {

// -----------------------------------------------------------------------------
// FAssetLockingStub: IAssetLockingBackend の null-object 実装
// -----------------------------------------------------------------------------
// IsConnected() は常に false (header で inline 定義済み)。
// 各操作は ACS_ERR(Generic, kSub_NotImplemented, ...) を返す。
// -----------------------------------------------------------------------------

TResult<void> FAssetLockingStub::LockAsset(const char* asset_path, const char* user) noexcept {
    (void)asset_path;
    (void)user;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::LockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

TResult<void> FAssetLockingStub::UnlockAsset(const char* asset_path) noexcept {
    (void)asset_path;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::UnlockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

TResult<FAssetLockInfo> FAssetLockingStub::QueryLock(const char* asset_path) noexcept {
    (void)asset_path;
    return TResult<FAssetLockInfo>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IAssetLockingBackend::QueryLock is not implemented "
                "(stub: link a concrete asset locking backend such as Perforce/Plastic)"));
}

// -----------------------------------------------------------------------------
// FBuildFarmStub: IBuildFarmBackend の null-object 実装
// -----------------------------------------------------------------------------
// IsConnected() は常に false (header で inline 定義済み)。
// 各操作は ACS_ERR(Generic, kSub_NotImplemented, ...) を返す。
// -----------------------------------------------------------------------------

TResult<u64> FBuildFarmStub::SubmitBuild(const FBuildRequest& req) noexcept {
    (void)req;
    return TResult<u64>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::SubmitBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

TResult<IBuildFarmBackend::FBuildResult> FBuildFarmStub::PollBuild(u64 build_id) noexcept {
    (void)build_id;
    return TResult<IBuildFarmBackend::FBuildResult>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::PollBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

TResult<void> FBuildFarmStub::CancelBuild(u64 build_id) noexcept {
    (void)build_id;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IBuildFarmBackend::CancelBuild is not implemented "
                   "(stub: link a concrete build farm backend such as Jenkins/TeamCity)");
}

// -----------------------------------------------------------------------------
// アクセサ: function-local static で process-wide singleton を保持
// -----------------------------------------------------------------------------
// C++11 magic statics により初期化は thread-safe。
// 破棄順序は他の static と独立で良い (相互依存なし)。
// -----------------------------------------------------------------------------

IAssetLockingBackend& GetAssetLockingStub() noexcept {
    static FAssetLockingStub s_instance;
    return s_instance;
}

IBuildFarmBackend& GetBuildFarmStub() noexcept {
    static FBuildFarmStub s_instance;
    return s_instance;
}

} // namespace acs::game
