// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar W — StudioWorkflow seam (IAssetLockingBackend / IBuildFarmBackend)
//
// 役割:
//   スタジオ運用 (チーム開発 / CI/CD) のために必要な 2 つの外部システムへ橋渡しする
//   **シーム (seam) インターフェース** を提供する。
//   ・**AssetLocking**: Perforce (P4) / Plastic SCM / Helix Core / Unity Accelerator
//     等の「アセット排他ロック」を抽象化。バイナリアセット (.fbx / .psd / .wav 等) を
//     複数人で同時編集して conflict を起こさないため、編集前にサーバ側で排他ロックを
//     取得する運用が AAA / 中規模スタジオで一般的。
//   ・**BuildFarm**: Jenkins / TeamCity / GitHub Actions self-hosted / Incredibuild /
//     Unreal Build Service / Unity Cloud Build 等の「ビルドファーム」を抽象化。
//     プリセット + branch + commit を指定して非同期にビルドジョブを投入し、artifact
//     の URL を受け取る運用を seam として固定する。
//
// 使い方:
//   class StudioEditor {
//       acs::game::IAssetLockingBackend* _locks = nullptr;
//       acs::game::IBuildFarmBackend*    _farm  = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは PerforceAssetLocking / JenkinsBuildFarm を DI、
//           // 開発ビルドでは Stub。
//           _locks = &acs::game::GetAssetLockingStub();
//           _farm  = &acs::game::GetBuildFarmStub();
//       }
//       void OnOpenAsset(const char* path) noexcept {
//           (void)_locks->LockAsset(path, "designer_a");
//       }
//       void OnRequestNightlyBuild() noexcept {
//           acs::game::IBuildFarmBackend::BuildRequest req{
//               "Shipping_Win64", "main", "1e6c12b"};
//           (void)_farm->SubmitBuild(req);
//       }
//   };
//
// 設計選択 (Pillar W Phase 1):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: Perforce / Plastic / Jenkins SDK は
//     プラットフォーム別 lib 配布 (libp4api.a など) で依存追加が重い。本体は SDK
//     非依存のままビルドできるよう、ヘッダだけは常に提供し、実装は別モジュール
//     (将来の `acs_perforce` / `acs_jenkins` 等) で override する形を取る。
//   ・**cross-backend で同じ I/F**: Perforce / Plastic / Helix / Git LFS のいずれを
//     後ろで使ってもエディタ側コードを書き換えない。アセットパスは `const char*
//     asset_path` の opaque な depot/relative path 文字列として渡す (例:
//     "//depot/Game/Art/Characters/hero.fbx")。
//   ・**所有しない const char*** : 文字列は呼び出し側 / 外部 SDK のライフタイムに
//     従う。Backend はコピーしない (STL <string> 不使用方針)。利用側は
//     `QueryLock()` の戻り値を「ティック内のみ有効」と扱うこと。
//   ・**Result<T, ErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は IsConnected
//     が常に false で、各操作は `ACS_ERR(Generic, kSub_NotImplemented, "...")` を
//     返す (BackendClient / SteamworksBridge と同じ defensive pattern)。
//   ・**Build ID は不透明 u64**: SubmitBuild の戻り値 / PollBuild の入力。実装側で
//     ジョブ番号 (連番) / hash / pointer-as-u64 等の意味を持たせてよいが、呼出側は
//     opaque ID として扱う。`0` は「無効な ID」予約 (StartSearch ticket と同じ約束)。
//   ・**Stub は static singleton で取得**: 依存ゼロのデフォルト実装として
//     `GetAssetLockingStub()` / `GetBuildFarmStub()` を提供。実 SDK 未統合の
//     ビルドでも `_locks = &GetAssetLockingStub();` だけでコンパイル可能。
//   ・**実 SDK 実装はここでは作らない**: PerforceAssetLocking / JenkinsBuildFarm 等は
//     外部 SDK 依存を伴うため、本ファイルでは I/F + Stub のみ。
//
// 範囲外 (Phase 2+ で):
//   ・ロック取得のリトライ / 自動再接続 / 切断検知の callback。
//   ・ビルドログのストリーミング (artifact url 経由で取得する想定)。
//   ・コードレビュー / PR 連携 (Pillar W の別 seam で扱う候補)。
//   ・アーティスト向け P4V/PlasticGUI 連携 (本 seam ではプログラム API のみ)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// 共通: stub 用エラーサブコード
// -----------------------------------------------------------------------------
// BackendClient / SaveSlot 等と同じく「Phase 1 stub = NotImplemented」を
// `subcode = kSub_NotImplemented (= 99)` で表現する。`ErrCategory` には Generic を
// 使う (P4 / Jenkins は I/O だが、本 seam は API 抽象であって特定の通信路を
// 仮定しないため Generic が妥当)。
// =============================================================================
struct StudioWorkflowError {
    enum SubCode : u16 {
        kSub_NotConnected   = 1,  // バックエンドへ未接続のまま呼ばれた
        kSub_BadArgument    = 2,  // asset_path / user / req フィールドが nullptr
        kSub_NotFound       = 3,  // asset_path / build_id が見つからない
        kSub_AlreadyLocked  = 4,  // 別ユーザーが既にロック保持
        kSub_PermissionDenied = 5,// バックエンド側で権限拒否
        kSub_NotImplemented = 99, // stub: 未実装
    };
};

// =============================================================================
// AssetLockInfo — QueryLock の戻り値 (P4 の `p4 fstat` 相当の最小情報)
// -----------------------------------------------------------------------------
// Backend は文字列を所有しない。`asset_path` / `locker_user` は外部 SDK 側
// (または Stub 内 static literal) のメモリを参照するだけで、呼び出し側で
// コピーしない。寿命は「次の Tick / 次の Backend 呼び出しまで」を保証する
// (実装によってはより長い)。
// =============================================================================
struct AssetLockInfo {
    const char* asset_path  = nullptr;  // ロック対象パス (例: "//depot/Game/foo.fbx")
    const char* locker_user = nullptr;  // ロック保持ユーザー (例: "designer_a")
    u64         lock_time   = 0;        // ロック取得時刻 (実装依存; UNIX epoch 推奨)
};

// =============================================================================
// IAssetLockingBackend — P4 / Plastic / Helix Core 等への抽象シーム
// -----------------------------------------------------------------------------
// ロックの粒度は「アセットパス 1 本につき 1 ロック」を想定。フォルダロックや
// 階層ロックは本 I/F ではサポートしない (具象実装側で実現するなら拡張)。
// =============================================================================
class IAssetLockingBackend {
public:
    IAssetLockingBackend() noexcept = default;
    virtual ~IAssetLockingBackend() noexcept = default;

    IAssetLockingBackend(const IAssetLockingBackend&)            = delete;
    IAssetLockingBackend& operator=(const IAssetLockingBackend&) = delete;
    IAssetLockingBackend(IAssetLockingBackend&&)                 = delete;
    IAssetLockingBackend& operator=(IAssetLockingBackend&&)      = delete;

    // 指定アセットを `user` の名前でロックする。既にロック中なら kSub_AlreadyLocked。
    // `asset_path` / `user` は呼出側が寿命保証する文字列 (string literal or member
    // バッファ)。Backend は内部でコピーしない。
    virtual Result<void> LockAsset(const char* asset_path, const char* user) noexcept = 0;

    // ロック解除。`asset_path` が未ロック状態なら kSub_NotFound。
    // 他ユーザーのロックを解除する権限は実装依存 (P4 では admin 権限が要る)。
    virtual Result<void> UnlockAsset(const char* asset_path) noexcept = 0;

    // 指定アセットの現在のロック状態を取得。未ロックなら kSub_NotFound。
    // 戻り値の文字列メンバの寿命は次の Backend 呼び出しまで保証する。
    virtual Result<AssetLockInfo> QueryLock(const char* asset_path) noexcept = 0;

    // バックエンドへ現在接続できているか。Stub は常に false。
    virtual bool IsConnected() const noexcept = 0;
};

// =============================================================================
// IBuildFarmBackend — Jenkins / TeamCity / Unity Cloud Build 等への抽象シーム
// -----------------------------------------------------------------------------
// 「ビルドを投入 → ポーリングで完了確認 → artifact URL を受け取る」フローのみを
// 抽象化する。リアルタイムログ / web hook 通知は本 I/F ではサポートしない。
// =============================================================================
class IBuildFarmBackend {
public:
    // ビルド投入リクエスト。フィールドはすべて呼出側が寿命保証する `const char*`。
    // `preset` は farm 側で予め登録されたビルド構成名 (例: "Shipping_Win64")、
    // `branch` は git/p4 のブランチ/ストリーム名、`commit_sha` はピンポイントの
    // リビジョン識別子 (git sha / p4 changelist 番号文字列 等)。
    struct BuildRequest {
        const char* preset     = nullptr;
        const char* branch     = nullptr;
        const char* commit_sha = nullptr;
    };

    // PollBuild の戻り値。`success == false` でもビルド ID は有効 (失敗を表す
    // 結果として返る)。`artifact_url` は成功時のみ非 nullptr を保証 (失敗時は
    // nullptr を返してよい)。寿命は次の Backend 呼び出しまで。
    struct BuildResult {
        u64         build_id     = 0;
        bool        success      = false;
        const char* artifact_url = nullptr;
    };

    IBuildFarmBackend() noexcept = default;
    virtual ~IBuildFarmBackend() noexcept = default;

    IBuildFarmBackend(const IBuildFarmBackend&)            = delete;
    IBuildFarmBackend& operator=(const IBuildFarmBackend&) = delete;
    IBuildFarmBackend(IBuildFarmBackend&&)                 = delete;
    IBuildFarmBackend& operator=(IBuildFarmBackend&&)      = delete;

    // ビルドを farm に投入。成功時は非ゼロの `build_id` を返す。
    // `req.preset` / `req.branch` / `req.commit_sha` のいずれかが nullptr なら
    // kSub_BadArgument を返す責務は具象実装側 (stub では NotImplemented を優先)。
    virtual Result<u64> SubmitBuild(const BuildRequest& req) noexcept = 0;

    // ビルド状態を取得。`build_id == 0` は kSub_BadArgument、未知の ID は kSub_NotFound。
    // 進行中は IsOk な BuildResult を返してもよい (success=false / artifact_url=nullptr)
    // が、本 I/F では「完了したかどうか」を呼出側に明示するため、進行中は IsErr を
    // 返す実装も許容する (kSub_NotFound 以外の Generic エラーで返すなど。具象側の
    // ポリシー)。
    virtual Result<BuildResult> PollBuild(u64 build_id) noexcept = 0;

    // ビルドのキャンセル要求。完了済み / 未知 ID への要求は kSub_NotFound。
    virtual Result<void> CancelBuild(u64 build_id) noexcept = 0;

    // バックエンドへ現在接続できているか。Stub は常に false。
    virtual bool IsConnected() const noexcept = 0;
};

// =============================================================================
// Stub 実装
// -----------------------------------------------------------------------------
// 外部 SDK (P4 / Jenkins 等) 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・IsConnected() は常に false。
//   ・各操作は ACS_ERR(Generic, kSub_NotImplemented, ...) を返す。
//   ・コピー/ムーブは基底 I/F で delete 済みのため本クラスも自然に non-copy。
// =============================================================================
class AssetLockingStub final : public IAssetLockingBackend {
public:
    AssetLockingStub() noexcept = default;
    ~AssetLockingStub() noexcept override = default;

    Result<void>           LockAsset(const char* asset_path, const char* user) noexcept override;
    Result<void>           UnlockAsset(const char* asset_path) noexcept override;
    Result<AssetLockInfo>  QueryLock(const char* asset_path) noexcept override;
    bool                   IsConnected() const noexcept override { return false; }
};

class BuildFarmStub final : public IBuildFarmBackend {
public:
    BuildFarmStub() noexcept = default;
    ~BuildFarmStub() noexcept override = default;

    Result<u64>          SubmitBuild(const BuildRequest& req) noexcept override;
    Result<BuildResult>  PollBuild(u64 build_id) noexcept override;
    Result<void>         CancelBuild(u64 build_id) noexcept override;
    bool                 IsConnected() const noexcept override { return false; }
};

// =============================================================================
// アクセサ: stub 実装への参照を取る
// -----------------------------------------------------------------------------
// 本体側 (タイトル / エディタ) はまずこの 2 つを使ってリンクを通す。
// 具象実装に切り替える際は `IAssetLockingBackend*` / `IBuildFarmBackend*` を持つ
// メンバ変数に `PerforceAssetLocking` / `JenkinsBuildFarm` 等を差し替える。
// =============================================================================

// プロセス共有の stub IAssetLockingBackend。常に NotImplemented を返す。
IAssetLockingBackend& GetAssetLockingStub() noexcept;

// プロセス共有の stub IBuildFarmBackend。常に NotImplemented を返す。
IBuildFarmBackend& GetBuildFarmStub() noexcept;

} // namespace acs::game
