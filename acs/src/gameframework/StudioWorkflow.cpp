// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar W — FStudioWorkflow stub 実装 (FAssetLockingStub / FBuildFarmStub)
//
// 本ファイルは FStudioWorkflow.h で宣言した 2 つの interface に対し、
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
#include "foundation/Platform.h"   // <windows.h> (CreateFileW / CreateProcessW 等)

namespace acs::game {

/** stub: 常に NotImplemented を返す (具象アセットロックバックエンドを link せよ)。 */
TResult<void> FAssetLockingStub::LockAsset(const char* asset_path, const char* user) noexcept {
    (void)asset_path;
    (void)user;
    return ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::LockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

/** stub: 常に NotImplemented を返す。 */
TResult<void> FAssetLockingStub::UnlockAsset(const char* asset_path) noexcept {
    (void)asset_path;
    return ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::UnlockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

/** stub: 常に NotImplemented を返す。 */
TResult<FAssetLockInfo> FAssetLockingStub::QueryLock(const char* asset_path) noexcept {
    (void)asset_path;
    return TResult<FAssetLockInfo>(
        ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                "IAssetLockingBackend::QueryLock is not implemented "
                "(stub: link a concrete asset locking backend such as Perforce/Plastic)"));
}

/** stub: 常に NotImplemented を返す (具象ビルドファームバックエンドを link せよ)。 */
TResult<u64> FBuildFarmStub::SubmitBuild(const BuildRequest& req) noexcept {
    (void)req;
    return TResult<u64>(
        ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::SubmitBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

/** stub: 常に NotImplemented を返す。 */
TResult<IBuildFarmBackend::BuildResult> FBuildFarmStub::PollBuild(u64 build_id) noexcept {
    (void)build_id;
    return TResult<IBuildFarmBackend::BuildResult>(
        ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::PollBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

/** stub: 常に NotImplemented を返す。 */
TResult<void> FBuildFarmStub::CancelBuild(u64 build_id) noexcept {
    (void)build_id;
    return ACS_ERR(Generic, StudioWorkflowError::kSub_NotImplemented,
                   "IBuildFarmBackend::CancelBuild is not implemented "
                   "(stub: link a concrete build farm backend such as Jenkins/TeamCity)");
}

namespace {

/**
 * UTF-8 文字列を UTF-16 へ変換する (FHotReload と同流儀)。
 *
 * @details u8 (NUL 終端) を out_w に NUL 終端付きで変換する。
 * @param u8 入力 UTF-8 文字列 (NUL 終端)。
 * @param out_w 変換結果を書き込む UTF-16 バッファ。
 * @param out_cap out_w の要素数。
 * @return 成功で true、入力 null / バッファ不足 / 変換失敗で false。
 */
bool Utf8ToUtf16(const char* u8, wchar_t* out_w, int out_cap) noexcept {
    if (u8 == nullptr || out_w == nullptr || out_cap <= 0) {
        return false;
    }
    // -1 を渡して入力 NUL 終端を含めて変換させる (出力も NUL 終端になる)。
    const int got = ::MultiByteToWideChar(CP_UTF8, 0, u8, -1, out_w, out_cap);
    return got > 0;
}

/**
 * char バッファへ NUL 終端付きで安全コピーする (STL 非依存)。
 *
 * @details src が長すぎれば dst の容量に合わせて切り詰める。
 * @param dst コピー先バッファ。
 * @param cap dst の要素数。
 * @param src コピー元文字列 (nullptr 可)。
 */
void CopyCStr(char* dst, int cap, const char* src) noexcept {
    if (dst == nullptr || cap <= 0) return;
    int i = 0;
    if (src != nullptr) {
        for (; i < cap - 1 && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/**
 * NUL 終端 C 文字列の一致を判定する (strcmp 相当, STL 非依存)。
 *
 * @param a 比較する文字列 1 (nullptr 可)。
 * @param b 比較する文字列 2 (nullptr 可)。
 * @return 内容が一致すれば true (両方 nullptr も一致扱い)。
 */
bool CStrEqual(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

/**
 * NUL 終端済み wchar_t バッファの末尾に suffix を連結する (NUL 終端維持)。
 *
 * @details 連結後に容量を超える場合は何もしない。
 * @param dst 連結先バッファ (NUL 終端済み)。
 * @param cap dst の要素数。
 * @param suffix 連結する UTF-16 文字列。
 * @return 連結に成功すれば true、溢れ/不正引数なら false。
 */
bool AppendWStr(wchar_t* dst, int cap, const wchar_t* suffix) noexcept {
    if (dst == nullptr || suffix == nullptr || cap <= 0) return false;
    int len = 0;
    while (len < cap && dst[len] != L'\0') ++len;
    if (len >= cap) return false;  // 終端が無い (壊れている)
    int j = 0;
    while (suffix[j] != L'\0') {
        if (len + 1 >= cap) return false;  // NUL の分を残せない
        dst[len++] = suffix[j++];
    }
    dst[len] = L'\0';
    return true;
}

/**
 * asset_path (UTF-8) を "<asset>.lock" の UTF-16 path に変換する。
 *
 * @param asset_path 入力アセットパス (UTF-8)。
 * @param out_w lock path を書き込む UTF-16 バッファ。
 * @param out_cap out_w の要素数。
 * @return 成功で true (out_w に NUL 終端付き lock path)、失敗で false。
 */
bool MakeLockPath(const char* asset_path, wchar_t* out_w, int out_cap) noexcept {
    if (!Utf8ToUtf16(asset_path, out_w, out_cap)) return false;
    return AppendWStr(out_w, out_cap, L".lock");
}

/**
 * 現在時刻を UNIX epoch 秒で取得する。
 *
 * @details Win32 FILETIME (1601-01-01 起点, 100ns 単位) を UNIX epoch 秒へ変換する。
 * @return 現在時刻の UNIX epoch 秒。
 */
u64 NowUnixSeconds() noexcept {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    u64 ticks = (static_cast<u64>(ft.dwHighDateTime) << 32) |
                static_cast<u64>(ft.dwLowDateTime);
    // 1601→1970 のオフセット (秒) = 11644473600。100ns → 秒は /10,000,000。
    constexpr u64 kEpochOffsetSec = 11644473600ull;
    return (ticks / 10000000ull) - kEpochOffsetSec;
}

/**
 * u64 を 10 進 ASCII 文字列に変換する (sprintf 非使用)。
 *
 * @param v 変換する値。
 * @param out 結果を書き込むバッファ (NUL 終端付き)。
 * @param cap out の要素数。
 * @return 書いたバイト数 (NUL 除く)。
 */
int U64ToDec(u64 v, char* out, int cap) noexcept {
    if (out == nullptr || cap <= 0) return 0;
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10ull));
        v /= 10ull;
    } while (v != 0ull && n < 24);
    int w = 0;
    while (n > 0 && w < cap - 1) {
        out[w++] = tmp[--n];
    }
    out[w] = '\0';
    return w;
}

/**
 * 10 進 ASCII を u64 に変換する (atoi 相当, STL 非使用)。
 *
 * @details p から数字以外が来るまで読む。先頭の空白/改行はスキップする。
 * @param p 変換元の文字列先頭。
 * @param len 走査可能な最大文字数。
 * @return 変換した u64 値。
 */
u64 DecToU64(const char* p, usize len) noexcept {
    u64 v = 0;
    usize i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\r' || p[i] == '\n')) ++i;
    for (; i < len && p[i] >= '0' && p[i] <= '9'; ++i) {
        v = v * 10ull + static_cast<u64>(p[i] - '0');
    }
    return v;
}

/**
 * lock ファイル本体を読み込み、owner と取得時刻に分解する。
 *
 * @details
 * フォーマットは 1 行目 = owner、2 行目 = 取得時刻(10 進秒)。out_os_err には
 * GetLastError を入れる (ERROR_FILE_NOT_FOUND は「未ロック」を意味する)。
 * @param lock_path_w 読み込む lock ファイルの UTF-16 パス。
 * @param owner_out owner 名を書き込むバッファ。
 * @param owner_cap owner_out の要素数。
 * @param time_out 取得時刻 (UNIX epoch 秒) を書き込む先。
 * @param out_os_err 失敗時の GetLastError を書き込む先。
 * @return ファイル存在 + 読み取り成功で true、存在しない/失敗で false。
 */
bool ReadLockFile(const wchar_t* lock_path_w,
                  char*          owner_out,
                  int            owner_cap,
                  u64&           time_out,
                  DWORD&         out_os_err) noexcept {
    out_os_err = 0;
    time_out   = 0;
    if (owner_out != nullptr && owner_cap > 0) owner_out[0] = '\0';

    HANDLE h = ::CreateFileW(lock_path_w, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out_os_err = ::GetLastError();
        return false;
    }

    char  raw[1024] = {};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, raw, static_cast<DWORD>(sizeof(raw) - 1), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) {
        out_os_err = ::GetLastError();
        return false;
    }
    raw[got] = '\0';

    // 1 行目 = owner (改行まで)。2 行目 = time。
    int line_end = 0;
    while (line_end < static_cast<int>(got) &&
           raw[line_end] != '\n' && raw[line_end] != '\r') {
        ++line_end;
    }
    // owner を owner_out へコピー (改行を NUL に)。
    if (owner_out != nullptr && owner_cap > 0) {
        int n = 0;
        for (; n < line_end && n < owner_cap - 1; ++n) owner_out[n] = raw[n];
        owner_out[n] = '\0';
    }
    // 2 行目 (時刻) を探す。
    int p = line_end;
    while (p < static_cast<int>(got) && (raw[p] == '\n' || raw[p] == '\r')) ++p;
    if (p < static_cast<int>(got)) {
        time_out = DecToU64(raw + p, static_cast<usize>(static_cast<int>(got) - p));
    }
    return true;
}

} // namespace

/** `<asset_path>.lock` を CREATE_NEW で原子的に生成し、owner と取得時刻を書く。 */
TResult<void> FLocalFileAssetLocking::LockAsset(const char* asset_path,
                                                const char* user) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::LockAsset: asset_path is null/empty");
    }
    if (user == nullptr || user[0] == '\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::LockAsset: user is null/empty");
    }

    wchar_t lock_w[kMaxPathChars];
    if (!MakeLockPath(asset_path, lock_w, kMaxPathChars)) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::LockAsset: path too long / not valid UTF-8");
    }

    // CREATE_NEW は「既に存在したら原子的に失敗」。これがロックの肝。
    // 2 者が同時に来ても OS が排他を保証し、片方のみ成功する。
    HANDLE h = ::CreateFileW(lock_w, GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            return ACS_ERR_OS(IO, StudioWorkflowError::kSub_AlreadyLocked,
                              "FLocalFileAssetLocking::LockAsset: asset already locked", err);
        }
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::LockAsset: CreateFileW(CREATE_NEW) failed",
                          err);
    }

    // owner + 改行 + 時刻 をテキストで書く。
    char  body[kMaxUserChars + 32];
    int   off = 0;
    for (int i = 0; user[i] != '\0' && off < static_cast<int>(sizeof(body)) - 24; ++i) {
        body[off++] = user[i];
    }
    body[off++] = '\n';
    off += U64ToDec(NowUnixSeconds(), body + off,
                    static_cast<int>(sizeof(body)) - off);
    body[off++] = '\n';

    DWORD wrote = 0;
    const BOOL wok = ::WriteFile(h, body, static_cast<DWORD>(off), &wrote, nullptr);
    const DWORD werr = wok ? 0u : ::GetLastError();
    ::CloseHandle(h);
    if (!wok || wrote != static_cast<DWORD>(off)) {
        // 書き込み失敗時は中途半端な lock を残さないよう削除を試みる。
        ::DeleteFileW(lock_w);
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::LockAsset: WriteFile (lock body) failed",
                          werr);
    }
    return Ok();
}

/** lock ファイルを削除してロックを解除する (協調ロックなので誰でも解除可)。 */
TResult<void> FLocalFileAssetLocking::UnlockAsset(const char* asset_path) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::UnlockAsset: asset_path is null/empty");
    }

    wchar_t lock_w[kMaxPathChars];
    if (!MakeLockPath(asset_path, lock_w, kMaxPathChars)) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::UnlockAsset: path too long / not valid UTF-8");
    }

    // 未ロック (lock ファイル無し) なら kSub_NotFound。
    char  owner[kMaxUserChars] = {};
    u64   t = 0;
    DWORD rerr = 0;
    if (!ReadLockFile(lock_w, owner, kMaxUserChars, t, rerr)) {
        if (rerr == ERROR_FILE_NOT_FOUND || rerr == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                              "FLocalFileAssetLocking::UnlockAsset: asset is not locked", rerr);
        }
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::UnlockAsset: cannot read lock file", rerr);
    }

    // 本実装は協調ロックなので所有者検証は緩い (誰でも解除可)。lock ファイルを削除。
    if (!::DeleteFileW(lock_w)) {
        const DWORD derr = ::GetLastError();
        if (derr == ERROR_FILE_NOT_FOUND || derr == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                              "FLocalFileAssetLocking::UnlockAsset: lock vanished mid-unlock",
                              derr);
        }
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::UnlockAsset: DeleteFileW failed", derr);
    }
    return Ok();
}

/** owner を検証し、一致する場合のみ lock ファイルを削除する。 */
TResult<void> FLocalFileAssetLocking::UnlockAssetAs(const char* asset_path,
                                                    const char* user) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::UnlockAssetAs: asset_path is null/empty");
    }
    if (user == nullptr || user[0] == '\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::UnlockAssetAs: user is null/empty");
    }

    wchar_t lock_w[kMaxPathChars];
    if (!MakeLockPath(asset_path, lock_w, kMaxPathChars)) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalFileAssetLocking::UnlockAssetAs: path too long / not valid UTF-8");
    }

    // lock ファイルを読み、owner を検証する。
    char  owner[kMaxUserChars] = {};
    u64   t = 0;
    DWORD rerr = 0;
    if (!ReadLockFile(lock_w, owner, kMaxUserChars, t, rerr)) {
        if (rerr == ERROR_FILE_NOT_FOUND || rerr == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                              "FLocalFileAssetLocking::UnlockAssetAs: asset is not locked", rerr);
        }
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::UnlockAssetAs: cannot read lock file", rerr);
    }
    if (!CStrEqual(owner, user)) {
        // 別ユーザーのロックは解除しない。
        return ACS_ERR(IO, StudioWorkflowError::kSub_PermissionDenied,
                       "FLocalFileAssetLocking::UnlockAssetAs: lock owned by a different user");
    }

    if (!::DeleteFileW(lock_w)) {
        const DWORD derr = ::GetLastError();
        if (derr == ERROR_FILE_NOT_FOUND || derr == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                              "FLocalFileAssetLocking::UnlockAssetAs: lock vanished mid-unlock",
                              derr);
        }
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalFileAssetLocking::UnlockAssetAs: DeleteFileW failed", derr);
    }
    return Ok();
}

/** lock ファイルの存在と内容を読み取り、FAssetLockInfo に詰めて返す。 */
TResult<FAssetLockInfo>
FLocalFileAssetLocking::QueryLock(const char* asset_path) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        return TResult<FAssetLockInfo>(
            ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                    "FLocalFileAssetLocking::QueryLock: asset_path is null/empty"));
    }

    wchar_t lock_w[kMaxPathChars];
    if (!MakeLockPath(asset_path, lock_w, kMaxPathChars)) {
        return TResult<FAssetLockInfo>(
            ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                    "FLocalFileAssetLocking::QueryLock: path too long / not valid UTF-8"));
    }

    u64   t = 0;
    DWORD rerr = 0;
    if (!ReadLockFile(lock_w, m_QueryUserBuf, kMaxUserChars, t, rerr)) {
        if (rerr == ERROR_FILE_NOT_FOUND || rerr == ERROR_PATH_NOT_FOUND) {
            return TResult<FAssetLockInfo>(
                ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                           "FLocalFileAssetLocking::QueryLock: asset is not locked", rerr));
        }
        return TResult<FAssetLockInfo>(
            ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                       "FLocalFileAssetLocking::QueryLock: cannot read lock file", rerr));
    }

    // asset_path を member バッファに保持して戻り値から参照させる
    // (寿命 = 次の Backend 呼び出しまで、ヘッダ契約どおり)。
    CopyCStr(m_QueryPathBuf, kMaxPathChars, asset_path);

    FAssetLockInfo info{};
    info.asset_path  = m_QueryPathBuf;
    info.locker_user = m_QueryUserBuf;
    info.lock_time   = t;
    return TResult<FAssetLockInfo>(OkInit, info);
}

/** 追跡中のプロセス HANDLE をすべて閉じて破棄する (プロセス自体は kill しない)。 */
FLocalBuildRunner::~FLocalBuildRunner() noexcept {
    // 追跡中のプロセス HANDLE をすべて閉じる (プロセス自体は kill しない)。
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId != 0) {
            CloseJob(m_Jobs[i]);
        }
    }
}

/** build_id でジョブを引く (無ければ nullptr)。 */
FLocalBuildRunner::Job* FLocalBuildRunner::FindJob(u64 build_id) noexcept {
    if (build_id == 0) return nullptr;
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId == build_id) return &m_Jobs[i];
    }
    return nullptr;
}

/** プロセス HANDLE を閉じてスロットを空きに戻す。 */
void FLocalBuildRunner::CloseJob(Job& job) noexcept {
    if (job.m_Process != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(job.m_Process));
        job.m_Process = nullptr;
    }
    job.m_BuildId = 0;
    job.m_Finished = false;
    job.m_Success = false;
    job.m_ExitCode = 0;
    job.m_Artifact[0] = '\0';
}

/** command_line を CreateProcessW で起動し、完了まで待って終了コードを回収する。 */
TResult<void> FLocalBuildRunner::RunBuild(const wchar_t* command_line,
                                          u32&           out_exit_code,
                                          u32            timeout_ms) noexcept {
    out_exit_code = 0;
    if (command_line == nullptr || command_line[0] == L'\0') {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalBuildRunner::RunBuild: command_line is null/empty");
    }

    // CreateProcessW は lpCommandLine を書き換える可能性があるため可変バッファへ複写。
    wchar_t cmd[kMaxCmdChars];
    {
        int n = 0;
        for (; command_line[n] != L'\0' && n < kMaxCmdChars - 1; ++n) {
            cmd[n] = command_line[n];
        }
        if (command_line[n] != L'\0') {
            return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                           "FLocalBuildRunner::RunBuild: command_line too long");
        }
        cmd[n] = L'\0';
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                          "FLocalBuildRunner::RunBuild: CreateProcessW failed", err);
    }

    const DWORD wait_ms = (timeout_ms == 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wr == WAIT_TIMEOUT) {
        // タイムアウト時は kill して HANDLE を閉じる。
        ::TerminateProcess(pi.hProcess, 1u);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return ACS_ERR(IO, StudioWorkflowError::kSub_PermissionDenied,
                       "FLocalBuildRunner::RunBuild: build timed out");
    }
    if (wr != WAIT_OBJECT_0) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalBuildRunner::RunBuild: WaitForSingleObject failed", err);
    }

    DWORD code = 0;
    const BOOL gok = ::GetExitCodeProcess(pi.hProcess, &code);
    const DWORD gerr = gok ? 0u : ::GetLastError();
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (!gok) {
        return ACS_ERR_OS(IO, StudioWorkflowError::kSub_PermissionDenied,
                          "FLocalBuildRunner::RunBuild: GetExitCodeProcess failed", gerr);
    }

    out_exit_code = static_cast<u32>(code);
    return Ok();
}

/** command_line を UTF-8 で受け、UTF-16 へ変換して RunBuild に委譲する。 */
TResult<void> FLocalBuildRunner::RunBuildUtf8(const char* command_line,
                                              u32&        out_exit_code,
                                              u32         timeout_ms) noexcept {
    out_exit_code = 0;
    wchar_t cmd_w[kMaxCmdChars];
    if (!Utf8ToUtf16(command_line, cmd_w, kMaxCmdChars)) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalBuildRunner::RunBuildUtf8: command_line null / too long / bad UTF-8");
    }
    return RunBuild(cmd_w, out_exit_code, timeout_ms);
}

/** preset を起動コマンドラインとして解釈し、非同期にビルドジョブを起動する。 */
TResult<u64> FLocalBuildRunner::SubmitBuild(const BuildRequest& req) noexcept {
    // preset を「起動するコマンドライン」として解釈する (ローカルファーム規約)。
    if (req.preset == nullptr || req.preset[0] == '\0') {
        return TResult<u64>(
            ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                    "FLocalBuildRunner::SubmitBuild: req.preset (command line) is null/empty"));
    }

    // 空きスロットを探す。
    Job* slot = nullptr;
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId == 0) { slot = &m_Jobs[i]; break; }
    }
    if (slot == nullptr) {
        return TResult<u64>(
            ACS_ERR(IO, StudioWorkflowError::kSub_PermissionDenied,
                    "FLocalBuildRunner::SubmitBuild: job table full"));
    }

    wchar_t cmd[kMaxCmdChars];
    if (!Utf8ToUtf16(req.preset, cmd, kMaxCmdChars)) {
        return TResult<u64>(
            ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                    "FLocalBuildRunner::SubmitBuild: preset too long / bad UTF-8"));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        return TResult<u64>(
            ACS_ERR_OS(IO, StudioWorkflowError::kSub_NotFound,
                       "FLocalBuildRunner::SubmitBuild: CreateProcessW failed", err));
    }
    ::CloseHandle(pi.hThread);  // メインスレッド HANDLE は不要。

    const u64 id = m_NextBuildId++;
    slot->m_BuildId  = id;
    slot->m_Process  = static_cast<void*>(pi.hProcess);
    slot->m_Finished = false;
    slot->m_Success  = false;
    slot->m_ExitCode = 0;
    // 疑似 artifact パス: branch/commit を含めて識別子化 (情報のみ)。
    {
        int off = 0;
        const char prefix[] = "local-build://job/";
        for (int i = 0; prefix[i] != '\0' && off < kMaxArtifactLen - 1; ++i) {
            slot->m_Artifact[off++] = prefix[i];
        }
        off += U64ToDec(id, slot->m_Artifact + off, kMaxArtifactLen - off);
        slot->m_Artifact[off] = '\0';
    }
    return TResult<u64>(OkInit, id);
}

/** ジョブの完了状態を非ブロッキングに確認し、結果を返す。 */
TResult<IBuildFarmBackend::BuildResult>
FLocalBuildRunner::PollBuild(u64 build_id) noexcept {
    if (build_id == 0) {
        return TResult<BuildResult>(
            ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                    "FLocalBuildRunner::PollBuild: build_id == 0 is reserved/invalid"));
    }
    Job* job = FindJob(build_id);
    if (job == nullptr) {
        return TResult<BuildResult>(
            ACS_ERR(IO, StudioWorkflowError::kSub_NotFound,
                    "FLocalBuildRunner::PollBuild: unknown build_id"));
    }

    // 未だラッチしていなければプロセス状態を非ブロッキングに確認する。
    if (!job->m_Finished) {
        HANDLE h = static_cast<HANDLE>(job->m_Process);
        const DWORD wr = ::WaitForSingleObject(h, 0);  // 0 = poll
        if (wr == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (::GetExitCodeProcess(h, &code)) {
                job->m_ExitCode = static_cast<u32>(code);
                job->m_Success  = (code == 0);
            }
            job->m_Finished = true;
        }
        // WAIT_TIMEOUT = まだ実行中。
    }

    BuildResult r{};
    r.build_id     = build_id;
    r.success      = job->m_Finished && job->m_Success;
    r.artifact_url = (job->m_Finished && job->m_Success) ? job->m_Artifact : nullptr;

    if (!job->m_Finished) {
        // 進行中は IsErr で返す (ヘッダが許容するポリシー)。Generic + NotFound 以外。
        return TResult<BuildResult>(
            ACS_ERR(Generic, StudioWorkflowError::kSub_PermissionDenied,
                    "FLocalBuildRunner::PollBuild: build still running"));
    }
    return TResult<BuildResult>(OkInit, r);
}

/** 進行中のジョブを kill してスロットを解放する。 */
TResult<void> FLocalBuildRunner::CancelBuild(u64 build_id) noexcept {
    if (build_id == 0) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_BadArgument,
                       "FLocalBuildRunner::CancelBuild: build_id == 0 is reserved/invalid");
    }
    Job* job = FindJob(build_id);
    if (job == nullptr) {
        return ACS_ERR(IO, StudioWorkflowError::kSub_NotFound,
                       "FLocalBuildRunner::CancelBuild: unknown build_id");
    }
    if (job->m_Finished) {
        // 既に完了済みのジョブはキャンセル不可 (NotFound 扱い)。
        return ACS_ERR(IO, StudioWorkflowError::kSub_NotFound,
                       "FLocalBuildRunner::CancelBuild: build already finished");
    }

    HANDLE h = static_cast<HANDLE>(job->m_Process);
    if (h != nullptr) {
        // まだ実行中なら kill。
        const DWORD wr = ::WaitForSingleObject(h, 0);
        if (wr == WAIT_TIMEOUT) {
            ::TerminateProcess(h, 1u);
        }
    }
    CloseJob(*job);
    return Ok();
}

/** function-local static で process-wide singleton の stub locking backend を返す。 */
IAssetLockingBackend& GetAssetLockingStub() noexcept {
    static FAssetLockingStub s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の stub build farm backend を返す。 */
IBuildFarmBackend& GetBuildFarmStub() noexcept {
    static FBuildFarmStub s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の実ローカル locking backend を返す。 */
FLocalFileAssetLocking& GetLocalFileAssetLocking() noexcept {
    static FLocalFileAssetLocking s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の実ローカル build runner を返す。 */
FLocalBuildRunner& GetLocalBuildRunner() noexcept {
    static FLocalBuildRunner s_instance;
    return s_instance;
}

} // namespace acs::game
