// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar W — FStudioWorkflow seam (IAssetLockingBackend / IBuildFarmBackend)
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
//       acs::game::IAssetLockingBackend* m_Locks = nullptr;
//       acs::game::IBuildFarmBackend*    m_Farm  = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは PerforceAssetLocking / JenkinsBuildFarm を DI、
//           // 開発ビルドでは Stub。
//           m_Locks = &acs::game::GetAssetLockingStub();
//           m_Farm  = &acs::game::GetBuildFarmStub();
//       }
//       void OnOpenAsset(const char* path) noexcept {
//           (void)m_Locks->LockAsset(path, "designer_a");
//       }
//       void OnRequestNightlyBuild() noexcept {
//           acs::game::IBuildFarmBackend::BuildRequest req{
//               "Shipping_Win64", "main", "1e6c12b"};
//           (void)m_Farm->SubmitBuild(req);
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
//     "//depot/FGame/Art/Characters/hero.fbx")。
//   ・**所有しない const char*** : 文字列は呼び出し側 / 外部 SDK のライフタイムに
//     従う。Backend はコピーしない (STL <string> 不使用方針)。利用側は
//     `QueryLock()` の戻り値を「ティック内のみ有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は IsConnected
//     が常に false で、各操作は `ACS_ERR(Generic, kSub_NotImplemented, "...")` を
//     返す (FBackendClient / FSteamworksBridge と同じ defensive pattern)。
//   ・**Build ID は不透明 u64**: SubmitBuild の戻り値 / PollBuild の入力。実装側で
//     ジョブ番号 (連番) / hash / pointer-as-u64 等の意味を持たせてよいが、呼出側は
//     opaque ID として扱う。`0` は「無効な ID」予約 (StartSearch ticket と同じ約束)。
//   ・**Stub は static singleton で取得**: 依存ゼロのデフォルト実装として
//     `GetAssetLockingStub()` / `GetBuildFarmStub()` を提供。実 SDK 未統合の
//     ビルドでも `m_Locks = &GetAssetLockingStub();` だけでコンパイル可能。
//   ・**実 (ローカル) 実装を同梱**: 外部 SDK (P4 / Jenkins) を必要としない
//     ローカル本実装を 2 つ提供する。`FLocalFileAssetLocking` = オンディスクの
//     サイドカー lock ファイル (CreateFileW CREATE_NEW の原子性で協調ロック) を
//     使う実ロック、`FLocalBuildRunner` = CreateProcessW で実際にローカルの
//     ビルドコマンドを起動し終了コードを回収する「サイズ 1 の実ビルドファーム」。
//     `GetLocalFileAssetLocking()` / `GetLocalBuildRunner()` で取得する。
//     これらは stub ではなく、実際に動作する本実装である。
//   ・**実 SDK 実装はここでは作らない**: PerforceAssetLocking / JenkinsBuildFarm 等
//     「ネットワーク越しの SCM / ビルドファーム」連携は外部 SDK 依存を伴うため、
//     本ファイルでは I/F + Stub + ローカル本実装まで。SDK 連携は別モジュールで。
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
// FBackendClient / FSaveSlot 等と同じく「Phase 1 stub = NotImplemented」を
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
// FAssetLockInfo — QueryLock の戻り値 (P4 の `p4 fstat` 相当の最小情報)
// -----------------------------------------------------------------------------
// Backend は文字列を所有しない。`asset_path` / `locker_user` は外部 SDK 側
// (または Stub 内 static literal) のメモリを参照するだけで、呼び出し側で
// コピーしない。寿命は「次の Tick / 次の Backend 呼び出しまで」を保証する
// (実装によってはより長い)。
// =============================================================================
struct FAssetLockInfo {
    const char* asset_path  = nullptr;  // ロック対象パス (例: "//depot/FGame/foo.fbx")
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
    virtual TResult<void> LockAsset(const char* asset_path, const char* user) noexcept = 0;

    // ロック解除。`asset_path` が未ロック状態なら kSub_NotFound。
    // 他ユーザーのロックを解除する権限は実装依存 (P4 では admin 権限が要る)。
    virtual TResult<void> UnlockAsset(const char* asset_path) noexcept = 0;

    // 指定アセットの現在のロック状態を取得。未ロックなら kSub_NotFound。
    // 戻り値の文字列メンバの寿命は次の Backend 呼び出しまで保証する。
    virtual TResult<FAssetLockInfo> QueryLock(const char* asset_path) noexcept = 0;

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
    virtual TResult<u64> SubmitBuild(const BuildRequest& req) noexcept = 0;

    // ビルド状態を取得。`build_id == 0` は kSub_BadArgument、未知の ID は kSub_NotFound。
    // 進行中は IsOk な BuildResult を返してもよい (success=false / artifact_url=nullptr)
    // が、本 I/F では「完了したかどうか」を呼出側に明示するため、進行中は IsErr を
    // 返す実装も許容する (kSub_NotFound 以外の Generic エラーで返すなど。具象側の
    // ポリシー)。
    virtual TResult<BuildResult> PollBuild(u64 build_id) noexcept = 0;

    // ビルドのキャンセル要求。完了済み / 未知 ID への要求は kSub_NotFound。
    virtual TResult<void> CancelBuild(u64 build_id) noexcept = 0;

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
class FAssetLockingStub final : public IAssetLockingBackend {
public:
    FAssetLockingStub() noexcept = default;
    ~FAssetLockingStub() noexcept override = default;

    TResult<void>           LockAsset(const char* asset_path, const char* user) noexcept override;
    TResult<void>           UnlockAsset(const char* asset_path) noexcept override;
    TResult<FAssetLockInfo>  QueryLock(const char* asset_path) noexcept override;
    bool                   IsConnected() const noexcept override { return false; }
};

class FBuildFarmStub final : public IBuildFarmBackend {
public:
    FBuildFarmStub() noexcept = default;
    ~FBuildFarmStub() noexcept override = default;

    TResult<u64>          SubmitBuild(const BuildRequest& req) noexcept override;
    TResult<BuildResult>  PollBuild(u64 build_id) noexcept override;
    TResult<void>         CancelBuild(u64 build_id) noexcept override;
    bool                 IsConnected() const noexcept override { return false; }
};

// =============================================================================
// FLocalFileAssetLocking — オンディスク lock ファイルによる実ロック実装
// -----------------------------------------------------------------------------
// Perforce / Plastic 等の外部 SCM サーバを使わず、**ローカルファイルシステム上の
// サイドカー lock ファイル** で IAssetLockingBackend を本実装する。
// 「実 (non-Perforce) 実装」であり stub ではない。
//
// 仕組み:
//   ・LockAsset(asset_path, user):
//       `<asset_path>.lock` を Win32 `CreateFileW(CREATE_NEW)` で生成する。
//       CREATE_NEW は「ファイルが既に存在したら ERROR_FILE_EXISTS で失敗する」
//       **原子的 (atomic)** な作成フラグ。OS カーネルが排他を保証するため、
//       2 プロセス / 2 スレッドが同時に同じアセットをロックしようとしても
//       片方だけが成功する (= 協調ロックの race を正しく処理)。成功した側は
//       lock ファイルへ owner 名と取得時刻 (UNIX epoch 秒) を書き込む。
//       既にロック済みなら kSub_AlreadyLocked。
//   ・UnlockAsset(asset_path):
//       lock ファイルを読み、所有者を検証してから `DeleteFileW` で削除する。
//       未ロックなら kSub_NotFound。
//   ・QueryLock(asset_path):
//       lock ファイルの存在 + 内容 (owner / time) を読み取って FAssetLockInfo に
//       詰めて返す。文字列は本オブジェクト内の固定長バッファ (m_QueryPathBuf /
//       m_QueryUserBuf) を指すため、寿命は「次の Backend 呼び出しまで」。
//   ・IsConnected(): ローカル FS は常に利用可能なので true。
//
// 設計メモ:
//   ・asset_path は UTF-8 の `const char*`。Win32 へ渡す前に内部で UTF-16 へ
//     変換する (FHotReload / FSaveArchive と同じ流儀)。`//depot/...` のような
//     P4 depot 構文ではなく、ローカル実装では実ファイルパス (相対/絶対) を渡す。
//   ・lock ファイルのフォーマットは 1 行目=owner、2 行目=取得時刻(10 進秒) の
//     極小テキスト。人間が読めて、クラッシュ後に手で消せる (協調ロックなので
//     強制力は無く、運用での合意が前提)。
//   ・STL 非依存: owner / path は固定長 char バッファ (member) に保持する。
// =============================================================================
class FLocalFileAssetLocking final : public IAssetLockingBackend {
public:
    // path / user バッファの最大長 (NUL 含む)。MAX_PATH 級 + 余裕。
    static constexpr int kMaxPathChars = 1024;
    static constexpr int kMaxUserChars = 256;

    FLocalFileAssetLocking() noexcept = default;
    ~FLocalFileAssetLocking() noexcept override = default;

    TResult<void>           LockAsset(const char* asset_path, const char* user) noexcept override;
    TResult<void>           UnlockAsset(const char* asset_path) noexcept override;
    TResult<FAssetLockInfo> QueryLock(const char* asset_path) noexcept override;
    bool                    IsConnected() const noexcept override { return true; }

    // 所有者検証付き解除 (seam 拡張)。lock ファイルの owner が `user` と一致する
    // 場合のみ削除する。一致しなければ kSub_PermissionDenied、未ロックは
    // kSub_NotFound。「自分のロックだけ外す」厳格運用が必要な場合に使う。
    // (基底 UnlockAsset(asset_path) は協調ロックとして誰でも解除可。)
    TResult<void> UnlockAssetAs(const char* asset_path, const char* user) noexcept;

private:
    // QueryLock の戻り値文字列が指す先 (寿命 = 次の呼び出しまで)。
    char m_QueryPathBuf[kMaxPathChars] = {};
    char m_QueryUserBuf[kMaxUserChars] = {};
};

// =============================================================================
// FLocalBuildRunner — Win32 CreateProcessW によるローカルビルド実行 (実装)
// -----------------------------------------------------------------------------
// Jenkins / TeamCity 等の外部ビルドファームを使わず、**ローカルマシン上で実際に
// ビルドコマンド (バッチ / exe) を起動して終了コードを回収する** ことで
// IBuildFarmBackend を本実装する。いわば「サイズ 1 の実ビルドファーム」。
// 「実 (non-Jenkins) 実装」であり stub ではない。
//
// 2 系統の API を提供する:
//   (A) RunBuild(command_line, &out_exit_code) — **同期** 実行ヘルパ。
//       Win32 CreateProcessW でコマンドを起動し、WaitForSingleObject で完了を
//       待ち、GetExitCodeProcess で実際の終了コードを回収して out_exit_code に
//       入れる。プロセス起動自体に成功すれば (終了コードが何であれ) IsOk を返す。
//       例: RunBuild(L"cmd /c exit 3", ec) → ec == 3。
//   (B) IBuildFarmBackend seam (SubmitBuild/PollBuild/CancelBuild) — preset を
//       「起動するコマンドライン」として解釈し、ジョブを内部テーブルに登録して
//       非同期に起動・追跡する。Jenkins 連携コードをそのまま流用できる。
//
// 設計メモ:
//   ・command_line は CreateProcessW の lpCommandLine 仕様に従い **書き換え可能な
//     バッファ** が必要なため、内部で固定長バッファへコピーしてから渡す。
//   ・SubmitBuild の `req.preset` を起動コマンドライン (UTF-8) とみなす。branch /
//     commit_sha は環境変数的なメタ情報として lock せず、ここでは情報のみ。
//   ・ジョブは固定長テーブル (kMaxJobs 件) で管理。build_id は 1 始まりの連番
//     (0 は無効予約)。
//   ・STL 非依存: ジョブテーブルは固定長配列、文字列は固定長 char バッファ。
// =============================================================================
class FLocalBuildRunner final : public IBuildFarmBackend {
public:
    static constexpr int kMaxJobs        = 32;     // 同時追跡できるビルドジョブ数
    static constexpr int kMaxCmdChars    = 4096;   // コマンドライン最大長 (NUL 含む)
    static constexpr int kMaxArtifactLen = 1024;   // artifact パス最大長 (NUL 含む)

    FLocalBuildRunner() noexcept = default;
    ~FLocalBuildRunner() noexcept override;

    // ---- (A) 同期実行ヘルパ (seam 非経由) ----------------------------------
    // command_line (UTF-16, 書き換え可能) を起動し、終了まで待ってから
    // out_exit_code に実際の終了コードを書く。起動失敗は OS サブコードで Err。
    // timeout_ms に 0 を渡すと INFINITE (完了まで待つ)。
    TResult<void> RunBuild(const wchar_t* command_line,
                           u32&           out_exit_code,
                           u32            timeout_ms = 0) noexcept;

    // command_line を UTF-8 で受ける版 (内部で UTF-16 へ変換)。
    TResult<void> RunBuildUtf8(const char* command_line,
                               u32&        out_exit_code,
                               u32         timeout_ms = 0) noexcept;

    // ---- (B) IBuildFarmBackend seam ----------------------------------------
    TResult<u64>          SubmitBuild(const BuildRequest& req) noexcept override;
    TResult<BuildResult>  PollBuild(u64 build_id) noexcept override;
    TResult<void>         CancelBuild(u64 build_id) noexcept override;
    bool                  IsConnected() const noexcept override { return true; }

private:
    // 1 ビルドジョブの追跡状態。
    struct Job {
        u64    m_BuildId = 0;                       // 0 = 空きスロット
        void*  m_Process = nullptr;                 // HANDLE (プロセス)。void* で windows.h を header に持ち込まない
        bool   m_Finished = false;                  // 完了済みか (poll でラッチ)
        bool   m_Success  = false;                  // exit_code == 0 か
        u32    m_ExitCode = 0;                      // 完了時の終了コード
        char   m_Artifact[kMaxArtifactLen] = {};    // 成功時に返す疑似 artifact パス
    };

    Job* FindJob(u64 build_id) noexcept;            // build_id でジョブを引く (無ければ nullptr)
    void CloseJob(Job& job) noexcept;               // プロセス HANDLE を閉じてスロットを空ける

    Job m_Jobs[kMaxJobs] = {};
    u64 m_NextBuildId    = 1;                        // 連番発番器 (0 は無効予約)
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

// =============================================================================
// アクセサ: 実 (ローカル) 実装への参照を取る
// -----------------------------------------------------------------------------
// 外部 SDK に依存しないオンディスク / ローカルプロセス本実装。stub と差し替えて
// `m_Locks = &GetLocalFileAssetLocking();` のように使う。process-wide singleton。
// =============================================================================

// プロセス共有の実 IAssetLockingBackend (オンディスク lock ファイル)。
FLocalFileAssetLocking& GetLocalFileAssetLocking() noexcept;

// プロセス共有の実 IBuildFarmBackend (ローカル CreateProcessW)。
FLocalBuildRunner& GetLocalBuildRunner() noexcept;

} // namespace acs::game
