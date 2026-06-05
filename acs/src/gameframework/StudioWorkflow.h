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
// 設計選択:
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
// 範囲外:
//   ・ロック取得のリトライ / 自動再接続 / 切断検知の callback。
//   ・ビルドログのストリーミング (artifact url 経由で取得する想定)。
//   ・コードレビュー / PR 連携 (別 seam で扱う候補)。
//   ・アーティスト向け P4V/PlasticGUI 連携 (本 seam ではプログラム API のみ)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * スタジオワークフロー seam 共通のエラーサブコード集。
 *
 * @details
 * FBackendClient / FSaveSlot 等と同じく「stub = NotImplemented」を
 * kSub_NotImplemented (= 99) で表現する。ErrCategory には Generic を使う (P4 /
 * Jenkins は I/O だが、本 seam は API 抽象であって特定の通信路を仮定しないため
 * Generic が妥当)。
 */
struct StudioWorkflowError {
    /** 各種失敗を表すサブコード。 */
    enum SubCode : u16 {
        /** バックエンドへ未接続のまま呼ばれた。 */
        kSub_NotConnected   = 1,

        /** asset_path / user / req フィールドが nullptr。 */
        kSub_BadArgument    = 2,

        /** asset_path / build_id が見つからない。 */
        kSub_NotFound       = 3,

        /** 別ユーザーが既にロックを保持している。 */
        kSub_AlreadyLocked  = 4,

        /** バックエンド側で権限拒否された。 */
        kSub_PermissionDenied = 5,

        /** stub: 未実装。 */
        kSub_NotImplemented = 99,
    };
};

/**
 * QueryLock の戻り値 (P4 の `p4 fstat` 相当の最小情報)。
 *
 * @details
 * Backend は文字列を所有しない。asset_path / locker_user は外部 SDK 側 (または Stub
 * 内 static literal) のメモリを参照するだけで、呼び出し側でコピーしない。寿命は
 * 「次の Tick / 次の Backend 呼び出しまで」を保証する (実装によってはより長い)。
 */
struct FAssetLockInfo {
    /** ロック対象パス (例: "//depot/FGame/foo.fbx")。Backend が所有しない借用文字列。 */
    const char* asset_path  = nullptr;

    /** ロック保持ユーザー (例: "designer_a")。Backend が所有しない借用文字列。 */
    const char* locker_user = nullptr;

    /** ロック取得時刻 (実装依存; UNIX epoch 推奨)。 */
    u64         lock_time   = 0;
};

/**
 * P4 / Plastic / Helix Core 等への抽象シーム (アセット排他ロック)。
 *
 * @details
 * ロックの粒度は「アセットパス 1 本につき 1 ロック」を想定。フォルダロックや階層
 * ロックは本 I/F ではサポートしない (具象実装側で実現するなら拡張)。
 */
class IAssetLockingBackend {
public:
    /** 既定構築。 */
    IAssetLockingBackend() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAssetLockingBackend() noexcept = default;

    /** コピー禁止 (バックエンドは単一所有を想定)。 */
    IAssetLockingBackend(const IAssetLockingBackend&)            = delete;

    /** コピー代入も禁止。 */
    IAssetLockingBackend& operator=(const IAssetLockingBackend&) = delete;

    /** ムーブ禁止。 */
    IAssetLockingBackend(IAssetLockingBackend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IAssetLockingBackend& operator=(IAssetLockingBackend&&)      = delete;

    /**
     * 指定アセットを user の名前でロックする。
     *
     * @details
     * 既にロック中なら kSub_AlreadyLocked。asset_path / user は呼出側が寿命保証する
     * 文字列 (string literal or member バッファ) で、Backend は内部でコピーしない。
     * @param asset_path ロック対象のアセットパス。
     * @param user ロック保持者の名前。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> LockAsset(const char* asset_path, const char* user) noexcept = 0;

    /**
     * 指定アセットのロックを解除する。
     *
     * @details
     * asset_path が未ロック状態なら kSub_NotFound。他ユーザーのロックを解除する権限は
     * 実装依存 (P4 では admin 権限が要る)。
     * @param asset_path ロック解除するアセットパス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> UnlockAsset(const char* asset_path) noexcept = 0;

    /**
     * 指定アセットの現在のロック状態を取得する。
     *
     * @details 未ロックなら kSub_NotFound。戻り値の文字列メンバの寿命は次の Backend 呼び出しまで保証する。
     * @param asset_path 問い合わせるアセットパス。
     * @return ロック情報、または未ロック/失敗時のエラー。
     */
    virtual TResult<FAssetLockInfo> QueryLock(const char* asset_path) noexcept = 0;

    /**
     * バックエンドへ現在接続できているかを返す。
     *
     * @return 接続済みなら true (Stub は常に false)。
     */
    virtual bool IsConnected() const noexcept = 0;
};

/**
 * Jenkins / TeamCity / Unity Cloud Build 等への抽象シーム (ビルドファーム)。
 *
 * @details
 * 「ビルドを投入 → ポーリングで完了確認 → artifact URL を受け取る」フローのみを抽象化
 * する。リアルタイムログ / web hook 通知は本 I/F ではサポートしない。
 */
class IBuildFarmBackend {
public:
    /**
     * ビルド投入リクエスト。
     *
     * @details
     * フィールドはすべて呼出側が寿命保証する const char*。preset は farm 側で予め登録
     * されたビルド構成名 (例: "Shipping_Win64")、branch は git/p4 のブランチ/ストリーム
     * 名、commit_sha はピンポイントのリビジョン識別子 (git sha / p4 changelist 番号
     * 文字列 等)。
     */
    struct BuildRequest {
        /** farm 側で予め登録されたビルド構成名 (例: "Shipping_Win64")。 */
        const char* preset     = nullptr;

        /** git/p4 のブランチ/ストリーム名。 */
        const char* branch     = nullptr;

        /** ピンポイントのリビジョン識別子 (git sha / p4 changelist 番号文字列 等)。 */
        const char* commit_sha = nullptr;
    };

    /**
     * PollBuild の戻り値。
     *
     * @details
     * success == false でもビルド ID は有効 (失敗を表す結果として返る)。artifact_url は
     * 成功時のみ非 nullptr を保証 (失敗時は nullptr を返してよい)。寿命は次の Backend
     * 呼び出しまで。
     */
    struct BuildResult {
        /** 対象ビルドの opaque ID (0 は無効予約)。 */
        u64         build_id     = 0;

        /** ビルドが成功したか。 */
        bool        success      = false;

        /** 成功時の artifact URL (失敗時は nullptr 可)。借用文字列。 */
        const char* artifact_url = nullptr;
    };

    /** 既定構築。 */
    IBuildFarmBackend() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IBuildFarmBackend() noexcept = default;

    /** コピー禁止 (バックエンドは単一所有を想定)。 */
    IBuildFarmBackend(const IBuildFarmBackend&)            = delete;

    /** コピー代入も禁止。 */
    IBuildFarmBackend& operator=(const IBuildFarmBackend&) = delete;

    /** ムーブ禁止。 */
    IBuildFarmBackend(IBuildFarmBackend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IBuildFarmBackend& operator=(IBuildFarmBackend&&)      = delete;

    /**
     * ビルドを farm に投入する。
     *
     * @details
     * 成功時は非ゼロの build_id を返す。req.preset / req.branch / req.commit_sha の
     * いずれかが nullptr なら kSub_BadArgument を返す責務は具象実装側 (stub では
     * NotImplemented を優先)。
     * @param req 投入するビルドリクエスト。
     * @return 成功時の build_id、または失敗時のエラー。
     */
    virtual TResult<u64> SubmitBuild(const BuildRequest& req) noexcept = 0;

    /**
     * ビルド状態を取得する。
     *
     * @details
     * build_id == 0 は kSub_BadArgument、未知の ID は kSub_NotFound。進行中は IsOk な
     * BuildResult を返してもよい (success=false / artifact_url=nullptr) が、本 I/F では
     * 「完了したかどうか」を呼出側に明示するため、進行中は IsErr を返す実装も許容する
     * (kSub_NotFound 以外の Generic エラーで返すなど。具象側のポリシー)。
     * @param build_id 状態を問い合わせるビルド ID。
     * @return ビルド結果、または失敗/進行中のエラー。
     */
    virtual TResult<BuildResult> PollBuild(u64 build_id) noexcept = 0;

    /**
     * ビルドのキャンセルを要求する。
     *
     * @details 完了済み / 未知 ID への要求は kSub_NotFound。
     * @param build_id キャンセルするビルド ID。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> CancelBuild(u64 build_id) noexcept = 0;

    /**
     * バックエンドへ現在接続できているかを返す。
     *
     * @return 接続済みなら true (Stub は常に false)。
     */
    virtual bool IsConnected() const noexcept = 0;
};

/**
 * IAssetLockingBackend の null-object stub 実装。
 *
 * @details
 * 外部 SDK (P4 / Plastic 等) 未統合ビルド / ユニットテスト用の no-op 実装。
 * IsConnected() は常に false、各操作は ACS_ERR(Generic, kSub_NotImplemented, ...) を
 * 返す。コピー/ムーブは基底 I/F で delete 済みのため本クラスも自然に non-copy。
 */
class FAssetLockingStub final : public IAssetLockingBackend {
public:
    /** 既定構築。 */
    FAssetLockingStub() noexcept = default;

    /** 破棄する。 */
    ~FAssetLockingStub() noexcept override = default;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param asset_path 無視される。
     * @param user 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<void>           LockAsset(const char* asset_path, const char* user) noexcept override;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param asset_path 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<void>           UnlockAsset(const char* asset_path) noexcept override;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param asset_path 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<FAssetLockInfo>  QueryLock(const char* asset_path) noexcept override;

    /**
     * 常に false を返す (未接続)。
     *
     * @return 常に false。
     */
    bool                   IsConnected() const noexcept override { return false; }
};

/**
 * IBuildFarmBackend の null-object stub 実装。
 *
 * @details
 * 外部 SDK (Jenkins / TeamCity 等) 未統合ビルド / ユニットテスト用の no-op 実装。
 * IsConnected() は常に false、各操作は ACS_ERR(Generic, kSub_NotImplemented, ...) を
 * 返す。
 */
class FBuildFarmStub final : public IBuildFarmBackend {
public:
    /** 既定構築。 */
    FBuildFarmStub() noexcept = default;

    /** 破棄する。 */
    ~FBuildFarmStub() noexcept override = default;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param req 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<u64>          SubmitBuild(const BuildRequest& req) noexcept override;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param build_id 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<BuildResult>  PollBuild(u64 build_id) noexcept override;

    /**
     * 常に NotImplemented を返す (no-op stub)。
     *
     * @param build_id 無視される。
     * @return kSub_NotImplemented エラー。
     */
    TResult<void>         CancelBuild(u64 build_id) noexcept override;

    /**
     * 常に false を返す (未接続)。
     *
     * @return 常に false。
     */
    bool                 IsConnected() const noexcept override { return false; }
};

/**
 * オンディスク lock ファイルによる IAssetLockingBackend の実ロック実装。
 *
 * @details
 * Perforce / Plastic 等の外部 SCM サーバを使わず、ローカルファイルシステム上の
 * サイドカー lock ファイルで本実装する (「実 (non-Perforce) 実装」であり stub では
 * ない)。LockAsset は `<asset_path>.lock` を Win32 CreateFileW(CREATE_NEW) で生成する
 * (CREATE_NEW は「既に存在したら ERROR_FILE_EXISTS で失敗」する原子的フラグで、OS
 * カーネルが排他を保証するため 2 プロセス/スレッドが同時にロックしても片方だけが成功
 * する)。成功側は lock ファイルへ owner 名と取得時刻 (UNIX epoch 秒) を書き込み、既に
 * ロック済みなら kSub_AlreadyLocked。UnlockAsset は lock ファイルを読んで所有者を検証
 * してから DeleteFileW で削除し (未ロックは kSub_NotFound)、QueryLock は lock ファイルの
 * 存在 + 内容を FAssetLockInfo に詰めて返す (文字列は本オブジェクト内の固定長バッファを
 * 指すため寿命は「次の Backend 呼び出しまで」)。IsConnected はローカル FS が常に利用
 * 可能なため true。asset_path は UTF-8 の const char* で内部で UTF-16 へ変換し、lock
 * ファイルは 1 行目=owner、2 行目=取得時刻(10 進秒) の極小テキスト。STL 非依存で owner
 * / path は固定長 char バッファに保持する。
 */
class FLocalFileAssetLocking final : public IAssetLockingBackend {
public:
    /** path バッファの最大長 (NUL 含む)。MAX_PATH 級 + 余裕。 */
    static constexpr int kMaxPathChars = 1024;

    /** user バッファの最大長 (NUL 含む)。 */
    static constexpr int kMaxUserChars = 256;

    /** 既定構築。 */
    FLocalFileAssetLocking() noexcept = default;

    /** 破棄する。 */
    ~FLocalFileAssetLocking() noexcept override = default;

    /**
     * `<asset_path>.lock` を CREATE_NEW で原子的に生成し、owner と取得時刻を書く。
     *
     * @details 既にロック済みなら kSub_AlreadyLocked。asset_path / user が空なら kSub_BadArgument。
     * @param asset_path ロック対象のアセットパス (UTF-8)。
     * @param user ロック保持者の名前 (UTF-8)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void>           LockAsset(const char* asset_path, const char* user) noexcept override;

    /**
     * lock ファイルを削除してロックを解除する (協調ロックなので誰でも解除可)。
     *
     * @details 未ロック (lock ファイル無し) なら kSub_NotFound。
     * @param asset_path ロック解除するアセットパス (UTF-8)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void>           UnlockAsset(const char* asset_path) noexcept override;

    /**
     * lock ファイルの存在と内容を読み取り、FAssetLockInfo に詰めて返す。
     *
     * @details
     * 未ロックなら kSub_NotFound。戻り値の文字列メンバは本オブジェクト内の固定長
     * バッファ (m_QueryPathBuf / m_QueryUserBuf) を指すため、寿命は「次の Backend
     * 呼び出しまで」。
     * @param asset_path 問い合わせるアセットパス (UTF-8)。
     * @return ロック情報、または未ロック/失敗時のエラー。
     */
    TResult<FAssetLockInfo> QueryLock(const char* asset_path) noexcept override;

    /**
     * ローカル FS は常に利用可能なので true を返す。
     *
     * @return 常に true。
     */
    bool                    IsConnected() const noexcept override { return true; }

    /**
     * 所有者検証付きでロックを解除する (seam 拡張)。
     *
     * @details
     * lock ファイルの owner が user と一致する場合のみ削除する。一致しなければ
     * kSub_PermissionDenied、未ロックは kSub_NotFound。「自分のロックだけ外す」厳格運用が
     * 必要な場合に使う (基底 UnlockAsset は協調ロックとして誰でも解除可)。
     * @param asset_path ロック解除するアセットパス (UTF-8)。
     * @param user 解除を要求するユーザー (lock の owner と照合)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> UnlockAssetAs(const char* asset_path, const char* user) noexcept;

private:
    /** QueryLock の戻り値 asset_path が指す先 (寿命 = 次の呼び出しまで)。 */
    char m_QueryPathBuf[kMaxPathChars] = {};

    /** QueryLock の戻り値 locker_user が指す先 (寿命 = 次の呼び出しまで)。 */
    char m_QueryUserBuf[kMaxUserChars] = {};
};

/**
 * Win32 CreateProcessW によるローカルビルド実行で IBuildFarmBackend を本実装する。
 *
 * @details
 * Jenkins / TeamCity 等の外部ビルドファームを使わず、ローカルマシン上で実際にビルド
 * コマンド (バッチ / exe) を起動して終了コードを回収する「サイズ 1 の実ビルドファーム」
 * (「実 (non-Jenkins) 実装」であり stub ではない)。2 系統の API を提供する: (A)
 * RunBuild(command_line, &out_exit_code) は同期実行ヘルパで、CreateProcessW で起動し
 * WaitForSingleObject で完了を待ち GetExitCodeProcess で終了コードを回収する (プロセス
 * 起動自体に成功すれば終了コードが何であれ IsOk)。(B) IBuildFarmBackend seam
 * (SubmitBuild/PollBuild/CancelBuild) は preset を「起動するコマンドライン」として解釈
 * し、ジョブを内部テーブルに登録して非同期に起動・追跡する。command_line は
 * CreateProcessW の lpCommandLine 仕様に従い書き換え可能なバッファが必要なため内部で
 * 固定長バッファへコピーしてから渡す。branch / commit_sha は情報のみ。ジョブは固定長
 * テーブル (kMaxJobs 件) で管理し、build_id は 1 始まりの連番 (0 は無効予約)。STL 非依存。
 */
class FLocalBuildRunner final : public IBuildFarmBackend {
public:
    /** 同時追跡できるビルドジョブ数。 */
    static constexpr int kMaxJobs        = 32;

    /** コマンドライン最大長 (NUL 含む)。 */
    static constexpr int kMaxCmdChars    = 4096;

    /** artifact パス最大長 (NUL 含む)。 */
    static constexpr int kMaxArtifactLen = 1024;

    /** 既定構築。 */
    FLocalBuildRunner() noexcept = default;

    /** 追跡中のプロセス HANDLE をすべて閉じて破棄する (プロセス自体は kill しない)。 */
    ~FLocalBuildRunner() noexcept override;

    /**
     * command_line (UTF-16, 書き換え可能) を起動し、終了まで待って終了コードを得る (同期、seam 非経由)。
     *
     * @details
     * 起動失敗は OS サブコードで Err を返す。timeout_ms に 0 を渡すと INFINITE
     * (完了まで待つ)。
     * @param command_line 起動するコマンドライン (UTF-16, 内部で可変バッファへ複写)。
     * @param out_exit_code プロセスの実際の終了コードを書き込む先。
     * @param timeout_ms 完了待ちのタイムアウト (ミリ秒、0 で INFINITE)。
     * @return 成功なら空の TResult、起動失敗/タイムアウトならエラー。
     */
    TResult<void> RunBuild(const wchar_t* command_line,
                           u32&           out_exit_code,
                           u32            timeout_ms = 0) noexcept;

    /**
     * command_line を UTF-8 で受け、内部で UTF-16 へ変換して RunBuild する。
     *
     * @param command_line 起動するコマンドライン (UTF-8)。
     * @param out_exit_code プロセスの実際の終了コードを書き込む先。
     * @param timeout_ms 完了待ちのタイムアウト (ミリ秒、0 で INFINITE)。
     * @return 成功なら空の TResult、変換/起動失敗ならエラー。
     */
    TResult<void> RunBuildUtf8(const char* command_line,
                               u32&        out_exit_code,
                               u32         timeout_ms = 0) noexcept;

    /**
     * preset を起動コマンドラインとして解釈し、非同期にビルドジョブを起動する。
     *
     * @details req.preset が空ならエラー、ジョブテーブルが満杯なら kSub_PermissionDenied。成功時は 1 始まりの連番 build_id を返す。
     * @param req 投入するビルドリクエスト (preset を起動コマンドラインとして使用)。
     * @return 成功時の build_id、または失敗時のエラー。
     */
    TResult<u64>          SubmitBuild(const BuildRequest& req) noexcept override;

    /**
     * ジョブの完了状態を非ブロッキングに確認し、結果を返す。
     *
     * @details build_id == 0 は kSub_BadArgument、未知 ID は kSub_NotFound、進行中は IsErr で返す。
     * @param build_id 状態を問い合わせるビルド ID。
     * @return ビルド結果、または失敗/進行中のエラー。
     */
    TResult<BuildResult>  PollBuild(u64 build_id) noexcept override;

    /**
     * 進行中のジョブを kill してスロットを解放する。
     *
     * @details 未知 ID / 完了済みジョブへの要求は kSub_NotFound。
     * @param build_id キャンセルするビルド ID。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void>         CancelBuild(u64 build_id) noexcept override;

    /**
     * ローカル実行は常に利用可能なので true を返す。
     *
     * @return 常に true。
     */
    bool                  IsConnected() const noexcept override { return true; }

private:
    /** 1 ビルドジョブの追跡状態。 */
    struct Job {
        /** ビルド ID (0 = 空きスロット)。 */
        u64    m_BuildId = 0;

        /** プロセス HANDLE。void* で windows.h を header に持ち込まない。 */
        void*  m_Process = nullptr;

        /** 完了済みか (poll でラッチされる)。 */
        bool   m_Finished = false;

        /** 成功したか (exit_code == 0 か)。 */
        bool   m_Success  = false;

        /** 完了時の終了コード。 */
        u32    m_ExitCode = 0;

        /** 成功時に返す疑似 artifact パス。 */
        char   m_Artifact[kMaxArtifactLen] = {};
    };

    /**
     * build_id でジョブを引く。
     *
     * @param build_id 検索するビルド ID。
     * @return 該当ジョブへのポインタ (無ければ nullptr)。
     */
    Job* FindJob(u64 build_id) noexcept;

    /**
     * プロセス HANDLE を閉じてスロットを空きに戻す。
     *
     * @param job 解放するジョブスロット。
     */
    void CloseJob(Job& job) noexcept;

    /** ビルドジョブ追跡テーブル (固定長)。 */
    Job m_Jobs[kMaxJobs] = {};

    /** 次に発番する build_id (1 始まりの連番、0 は無効予約)。 */
    u64 m_NextBuildId    = 1;
};

/**
 * プロセス共有の stub IAssetLockingBackend を返す。
 *
 * @details
 * 依存ゼロのデフォルト実装で、常に NotImplemented を返す。本体側 (タイトル / エディタ)
 * はまずこれを使ってリンクを通し、具象実装に切り替える際は IAssetLockingBackend* を持つ
 * メンバ変数に PerforceAssetLocking 等を差し替える。
 * @return プロセス共有の FAssetLockingStub への参照。
 */
IAssetLockingBackend& GetAssetLockingStub() noexcept;

/**
 * プロセス共有の stub IBuildFarmBackend を返す。
 *
 * @details 依存ゼロのデフォルト実装で、常に NotImplemented を返す。
 * @return プロセス共有の FBuildFarmStub への参照。
 */
IBuildFarmBackend& GetBuildFarmStub() noexcept;

/**
 * プロセス共有の実 IAssetLockingBackend (オンディスク lock ファイル) を返す。
 *
 * @details
 * 外部 SDK に依存しないオンディスク本実装。stub と差し替えて `m_Locks =
 * &GetLocalFileAssetLocking();` のように使う process-wide singleton。
 * @return プロセス共有の FLocalFileAssetLocking への参照。
 */
FLocalFileAssetLocking& GetLocalFileAssetLocking() noexcept;

/**
 * プロセス共有の実 IBuildFarmBackend (ローカル CreateProcessW) を返す。
 *
 * @details 外部 SDK に依存しないローカルプロセス本実装の process-wide singleton。
 * @return プロセス共有の FLocalBuildRunner への参照。
 */
FLocalBuildRunner& GetLocalBuildRunner() noexcept;

} // namespace acs::game
