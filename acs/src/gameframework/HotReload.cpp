// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FHotReload 実装 (dev build = 実 FS watcher)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・watched paths / callbacks / pending events の TArray に加え、監視ディレクトリ
//     ごとの OS watcher 状態 (FWatchEntry) を持つレジストリ。
//   ・Windows: WatchDirectory で CreateFileW(FILE_LIST_DIRECTORY,
//     FILE_FLAG_OVERLAPPED) してから ReadDirectoryChangesW を発行。Tick で
//     GetOverlappedResult(bWait=FALSE) によりノンブロッキングに完了を poll し、
//     FILE_NOTIFY_INFORMATION から変更 path と Action を取り出して event 化、
//     read を再発行して継続監視する。Shutdown で CancelIoEx + CloseHandle。
//   ・FIFO 順を保つため pending events は「先頭取り出し + shift-left」する
//     (swap-remove は順序を壊すので不採用)。OS 由来の path 文字列は m_EventPaths
//     (FString) が所有し、m_PendingEvents と lockstep で push / shift / clear する。
//   ・Ship build (`ACS_GAME_SHIPPING` 定義時) では全 method を no-op に。
//     ヘッダ側で member もろとも `#ifdef` で消えるため、`.cpp` 側も同じガードで
//     空の実装にする (シンボル定義は残し、呼び出し側コードが #ifdef だらけに
//     ならないようにする)。

#include "gameframework/HotReload.h"

#include "foundation/Log.h"

#ifndef ACS_GAME_SHIPPING
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include "foundation/Platform.h"   // <windows.h> (ReadDirectoryChangesW 等)
#include "foundation/Move.h"       // Move
#include "memory/UniquePtr.h"      // MakeUnique
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
#include "gameframework/HotReloadDiagnosticsInternal.h"
#endif
#endif

namespace acs::game {

// EHotReloadResult をログ・診断向けの安定した名前へ変換する。
const char* HotReloadResultName(EHotReloadResult result) noexcept {
    switch (result) {
    case EHotReloadResult::Success:           return "Success";
    case EHotReloadResult::AlreadyRegistered: return "AlreadyRegistered";
    case EHotReloadResult::NotRegistered:     return "NotRegistered";
    case EHotReloadResult::InvalidArgument:   return "InvalidArgument";
    case EHotReloadResult::InvalidUtf8:       return "InvalidUtf8";
    case EHotReloadResult::PathTooLong:       return "PathTooLong";
    case EHotReloadResult::LimitExceeded:     return "LimitExceeded";
    case EHotReloadResult::OutOfMemory:       return "OutOfMemory";
    case EHotReloadResult::OsError:           return "OsError";
    case EHotReloadResult::ReentrantCall:     return "ReentrantCall";
    case EHotReloadResult::NativeOverflow:    return "NativeOverflow";
    case EHotReloadResult::Count:             return "Unknown";
    }
    return "Unknown";
}

namespace hot_reload_detail {

// 診断 counter を最大値で飽和加算し、長時間稼働時の 0 への折り返しを防ぐ。
void SaturatingIncrement(u64& value) noexcept {
    if (value != ~static_cast<u64>(0)) {
        ++value;
    }
}

} // namespace hot_reload_detail

#ifndef ACS_GAME_SHIPPING

// 監視ディレクトリ 1 件あたりの ReadDirectoryChangesW 状態。
//
// HANDLE + OVERLAPPED + 受信バッファ + recursive フラグを束ねる。OVERLAPPED と m_Buffer
// のアドレスは I/O 発行から完了通知まで安定している必要があるため、この struct は
// TUniquePtr で個別 heap 確保される (ヘッダ参照)。
struct FWatchEntry {
    // ReadDirectoryChangesW の受信バッファのバイト数。
    //
    // FILE_NOTIFY_INFORMATION は DWORD 整列が要求されるため u32 配列で確保してアラインを
    // 保証する。32KiB あれば 1 フレーム分の連続変更には十分 (溢れたら次の read で取り直す)。
    static constexpr DWORD kBufferBytes = 32u * 1024u;

    // Unwatch の一致判定と event path 連結に使う所有 UTF-8 directory path。
    FString    m_Path;

    // CreateFileW で開いたディレクトリ HANDLE。
    HANDLE     m_Dir       = INVALID_HANDLE_VALUE;

    // サブディレクトリも監視するか。
    bool       m_Recursive = true;

    // ReadDirectoryChangesW 発行中か。
    bool       m_ReadPending = false;

    // 非同期 I/O 状態 (アドレス固定が必須)。
    OVERLAPPED m_Overlapped{};

    // DWORD 整列された受信バッファ。
    u32        m_Buffer[kBufferBytes / sizeof(u32)] = {};

    // 空状態で構築する (m_Dir は invalid、HANDLE は未確保)。
    FWatchEntry() noexcept = default;

    // 破棄する (Close で I/O 取り消し + HANDLE クローズ)。
    ~FWatchEntry() noexcept {
        Close();
    }

    // コピー禁止 (HANDLE / OVERLAPPED の所有を曖昧にしないため)。
    FWatchEntry(const FWatchEntry&)            = delete;

    // コピー代入も禁止。
    FWatchEntry& operator=(const FWatchEntry&) = delete;

    // ムーブ禁止。
    FWatchEntry(FWatchEntry&&)                 = delete;

    // ムーブ代入も禁止。
    FWatchEntry& operator=(FWatchEntry&&)      = delete;

    // 発行中の I/O を取り消し、HANDLE を閉じる (多重呼び出し安全)。
    void Close() noexcept {
        if (m_Dir != INVALID_HANDLE_VALUE) {
            // CancelIoEx は「取り消し要求」を出すだけで、OVERLAPPED と受信
            // バッファを直ちに解放してよい保証はない。完了通知を回収せずに
            // FWatchEntry を破棄すると、カーネルが解放済み m_Overlapped /
            // m_Buffer へ書き戻す可能性があるため、キャンセル完了まで待つ。
            if (m_ReadPending) {
                ::CancelIoEx(m_Dir, &m_Overlapped);
                DWORD ignored = 0;
                // 成功、または ERROR_OPERATION_ABORTED で戻れば I/O は完了済み。
                // CancelIoEx が ERROR_NOT_FOUND を返す競合でも、ここで既完了の
                // 結果を回収してからメモリを破棄する。
                (void)::GetOverlappedResult(m_Dir, &m_Overlapped, &ignored, TRUE);
                m_ReadPending = false;
            }
            ::CloseHandle(m_Dir);
            m_Dir = INVALID_HANDLE_VALUE;
        }
    }

    // ReadDirectoryChangesW を 1 回発行する (one-shot)。
    //
    // OVERLAPPED を毎回リセットし、completion routine を使わず poll 方式で待つ。
    // 発行に成功して m_ReadPending=true になれば true、失敗なら false を返す。
    bool IssueRead() noexcept {
        if (m_Dir == INVALID_HANDLE_VALUE) {
            return false;
        }
        // OVERLAPPED は read ごとにリセット (hEvent は使わず GetOverlappedResult で poll)。
        m_Overlapped = OVERLAPPED{};
        const DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                              FILE_NOTIFY_CHANGE_DIR_NAME  |
                              FILE_NOTIFY_CHANGE_LAST_WRITE |
                              FILE_NOTIFY_CHANGE_SIZE;
        const BOOL ok = ::ReadDirectoryChangesW(
            m_Dir,
            m_Buffer,
            kBufferBytes,
            m_Recursive ? TRUE : FALSE,
            kFilter,
            nullptr,            // bytes returned は overlapped read では未使用
            &m_Overlapped,
            nullptr);           // completion routine は使わない (poll 方式)
        if (ok == FALSE) {
            const DWORD err = ::GetLastError();
            ACS_LOG_WARN("HotReloadWatcher: ReadDirectoryChangesW failed (err=%lu)",
                         static_cast<unsigned long>(err));
            m_ReadPending = false;
            return false;
        }
        m_ReadPending = true;
        return true;
    }
};

namespace hot_reload_detail {

// native 通知を変換または queue へ追加する前に、信頼できる必要がある scalar field
// を検証する。
//
// 空 filename と、仕様で定められた FILE_ACTION_*（値 1〜5）以外の action は、
// 意図的に通知欠落として扱う。呼び出し側は OsError を返し、所有側へ
// authoritative rescan を要求する。
bool IsValidNativeNotificationFields(
    u32 action, usize file_name_bytes) noexcept {
    if (file_name_bytes == 0u ||
        (file_name_bytes % sizeof(WCHAR)) != 0u) {
        return false;
    }
    switch (action) {
    case FILE_ACTION_ADDED:
    case FILE_ACTION_REMOVED:
    case FILE_ACTION_MODIFIED:
    case FILE_ACTION_RENAMED_OLD_NAME:
    case FILE_ACTION_RENAMED_NEW_NAME:
        return true;
    default:
        return false;
    }
}

} // namespace hot_reload_detail

namespace {

// 1 回の TryTick で複数失敗を観測した場合の返却優先度。
//
// 診断の last_failure は観測順を維持し、この優先度は返却値の集約だけに使う。
u8 HotReloadResultPriority(EHotReloadResult result) noexcept {
    switch (result) {
    case EHotReloadResult::OsError:        return 5u;
    case EHotReloadResult::NativeOverflow: return 4u;
    case EHotReloadResult::OutOfMemory:    return 3u;
    case EHotReloadResult::LimitExceeded:  return 2u;
    case EHotReloadResult::Success:        return 0u;
    default:                               return 1u;
    }
}

// candidate の優先度が高い場合だけ、TryTick の代表返却値を更新する。
void AccumulateHotReloadResult(
    EHotReloadResult candidate, EHotReloadResult& aggregate) noexcept {
    if (HotReloadResultPriority(candidate) >
        HotReloadResultPriority(aggregate)) {
        aggregate = candidate;
    }
}

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)

// synthetic FILE_NOTIFY_INFORMATION を所有コピーする固定長 test seam。
constexpr usize kMaximumInjectedNativeBytes = 512u;

struct FInjectedHotReloadNativeState {
    internal::EHotReloadNativeFaultForTesting fault =
        internal::EHotReloadNativeFaultForTesting::None;
    usize byte_count = 0u;
    alignas(DWORD) u8 bytes[kMaximumInjectedNativeBytes] = {};
};

// HotReload 自体と同じ単一 thread 専用。製品 build には存在しない。
FInjectedHotReloadNativeState g_InjectedNativeState{};

#endif

// FILE_ACTION_* を「消えた」フラグへマップする。
//
// 削除 (FILE_ACTION_REMOVED) と rename-from (RENAMED_OLD_NAME) を removed 扱いにする。
// action は FILE_NOTIFY_INFORMATION の Action 値。削除系なら true を返す。
bool ActionIsRemoval(DWORD action) noexcept {
    return action == FILE_ACTION_REMOVED ||
           action == FILE_ACTION_RENAMED_OLD_NAME;
}

// WCHAR (UTF-16, 非 NUL 終端 + 文字数指定) を FString (UTF-8) へ変換する。
//
// 不正な native 文字列と長さ違反は OsError、確保失敗だけは OutOfMemory として
// 区別する。既存内容は常に最初に消去し、失敗時は空のままにする。
EHotReloadResult ConvertNativeUtf16ToUtf8(
    const wchar_t* w, int wlen, FString& out,
    bool force_out_of_memory = false) noexcept {
    out.Clear();
    if (w == nullptr || wlen <= 0) {
        return EHotReloadResult::OsError;
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wlen,
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0 ||
        static_cast<usize>(need) > CHotReloadWatcher::kMaxPathBytes) {
        return EHotReloadResult::OsError;
    }
    if (force_out_of_memory) {
        return EHotReloadResult::OutOfMemory;
    }
    // FString には生バッファ書き込み API が無いため、スタック上の小バッファに
    // 変換してから Append する。長い path も扱えるよう chunk せず一括 (need は
    // FILE_NOTIFY_INFORMATION の filename 由来で MAX_PATH 級に収まる想定)。
    char  stack_buf[1024];
    char* buf = stack_buf;
    FAllocator* alloc = nullptr;
    if (static_cast<usize>(need) >= sizeof(stack_buf)) {
        alloc = out.GetAllocator();
        buf = static_cast<char*>(alloc->Alloc(static_cast<usize>(need),
                                              alignof(char), FSourceLoc::Current()));
        if (buf == nullptr) {
            return EHotReloadResult::OutOfMemory;
        }
    }
    const int got = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wlen,
                                          buf, need, nullptr, nullptr);
    EHotReloadResult result = EHotReloadResult::OsError;
    if (got > 0) {
        result = out.TryAppend(FStringView(buf, static_cast<usize>(got)))
            ? EHotReloadResult::Success
            : EHotReloadResult::OutOfMemory;
    }
    if (alloc != nullptr) {
        alloc->Free(buf);
    }
    return result;
}

// UTF-8 の path を UTF-16 へ変換し、NUL 終端で out_w に書く。
//
// u8 は入力 UTF-8 文字列 (NUL 終端)、out_w は出力 UTF-16 バッファ、
// out_cap は out_w の WCHAR 単位の要素数。変換成功なら true、
// バッファ不足または変換失敗なら false を返す。
bool Utf8ToUtf16(
    const char* u8, usize u8_len, wchar_t* out_w, int out_cap) noexcept {
    if (u8 == nullptr || u8_len == 0u || u8_len > static_cast<usize>(INT_MAX) ||
        out_w == nullptr || out_cap <= 0) {
        return false;
    }
    const int got = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, u8, static_cast<int>(u8_len),
        out_w, out_cap - 1);
    if (got <= 0 || got >= out_cap) {
        return false;
    }
    out_w[got] = L'\0';
    return true;
}

EHotReloadResult ValidatePath(const char* path, usize& out_len) noexcept {
    out_len = 0u;
    if (path == nullptr || path[0] == '\0') {
        return EHotReloadResult::InvalidArgument;
    }
    while (out_len <= CHotReloadWatcher::kMaxPathBytes && path[out_len] != '\0') {
        const unsigned char c = static_cast<unsigned char>(path[out_len]);
        if (c < 0x20u) {
            return EHotReloadResult::InvalidArgument;
        }
        ++out_len;
    }
    if (out_len > CHotReloadWatcher::kMaxPathBytes) {
        return EHotReloadResult::PathTooLong;
    }
    const int converted = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(out_len),
        nullptr, 0);
    return converted > 0
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidUtf8;
}

// dir path に "/" 区切りで相対 path を連結して絶対 path に復元する。
//
// ReadDirectoryChangesW の filename は dir 相対なので、これで絶対 path に戻す。
// dir は監視 dir の UTF-8 path。空または nullptr なら rel_inout をそのまま使う。
// rel_inout は入力時に dir 相対 path、出力時に連結後の絶対 path (UTF-8) となる。
EHotReloadResult JoinNativePath(
    FStringView dir, FString& rel_inout) noexcept {
    if (rel_inout.IsEmpty()) {
        return EHotReloadResult::OsError;
    }
    const bool needs_separator =
        !dir.IsEmpty() &&
        dir[dir.Size() - 1u] != '/' &&
        dir[dir.Size() - 1u] != '\\';
    const usize separator_bytes = needs_separator ? 1u : 0u;
    if (dir.Size() > CHotReloadWatcher::kMaxPathBytes ||
        rel_inout.Size() > CHotReloadWatcher::kMaxPathBytes ||
        separator_bytes >
            CHotReloadWatcher::kMaxPathBytes - dir.Size() ||
        rel_inout.Size() >
            CHotReloadWatcher::kMaxPathBytes -
                dir.Size() - separator_bytes) {
        return EHotReloadResult::OsError;
    }

    FString joined;
    if (!dir.IsEmpty()) {
        if (!joined.TryAppend(dir)) {
            return EHotReloadResult::OutOfMemory;
        }
        if (needs_separator) {
            if (!joined.TryAppend('/')) {
                return EHotReloadResult::OutOfMemory;
            }
        }
    }
    if (!joined.TryAppend(rel_inout.View())) {
        return EHotReloadResult::OutOfMemory;
    }
    rel_inout = Move(joined);
    return EHotReloadResult::Success;
}

} // namespace

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)

namespace internal {

bool ConfigureHotReloadNativeFaultForTesting(
    EHotReloadNativeFaultForTesting fault,
    const void* bytes, usize byte_count) noexcept {
    using EF = EHotReloadNativeFaultForTesting;
    if (g_InjectedNativeState.fault != EF::None ||
        fault == EF::None) {
        return false;
    }

    const bool uses_synthetic_buffer =
        fault == EF::SyntheticBuffer ||
        fault == EF::SyntheticBufferConversionOutOfMemory;
    if (uses_synthetic_buffer) {
        if (bytes == nullptr || byte_count == 0u ||
            byte_count > kMaximumInjectedNativeBytes) {
            return false;
        }
    } else if (bytes != nullptr || byte_count != 0u) {
        return false;
    }

    g_InjectedNativeState.byte_count = byte_count;
    if (byte_count > 0u) {
        std::memcpy(g_InjectedNativeState.bytes, bytes, byte_count);
    }
    // fault は最後に設定し、途中状態を TryTick から観測させない。
    g_InjectedNativeState.fault = fault;
    return true;
}

void ResetHotReloadNativeFaultForTesting() noexcept {
    g_InjectedNativeState = FInjectedHotReloadNativeState{};
}

} // namespace internal

#endif

// 空状態で構築する (FWatchEntry の完全型が見える本 TU で定義)。
CHotReloadWatcher::CHotReloadWatcher() noexcept = default;

EHotReloadResult CHotReloadWatcher::HandleNativeOverflowCompletion(
    bool rearm_succeeded) noexcept {
    const EHotReloadResult result = rearm_succeeded
        ? EHotReloadResult::NativeOverflow
        : EHotReloadResult::OsError;
    RecordDiagnosticFailure(result, false, true);
    return result;
}

void CHotReloadWatcher::RecordDiagnosticFailure(
    EHotReloadResult result, bool rejected_event,
    bool notification_lost) noexcept {
    if (result == EHotReloadResult::Success) {
        return;
    }
    m_Diagnostics.last_failure = result;
    if (rejected_event) {
        hot_reload_detail::SaturatingIncrement(
            m_Diagnostics.rejected_event_count);
    }
    if (notification_lost) {
        hot_reload_detail::SaturatingIncrement(
            m_Diagnostics.loss_incident_count);
        m_Diagnostics.authoritative_rescan_required = true;
    }
}

// デストラクタ (FWatchEntry が完全型になる本 TU で実体化し Shutdown を呼ぶ)。
CHotReloadWatcher::~CHotReloadWatcher() noexcept {
    Shutdown();
}

// 内部バッファを軽く予約する (OS watcher の起動は WatchDirectory が担う)。
void CHotReloadWatcher::Init() noexcept {
    // 既に WatchDirectory 済みなら何もしない (多重 Init 安全)。実際の OS watcher
    // 起動 (CreateFileW + ReadDirectoryChangesW) は WatchDirectory が担う。
    // ここではバッファを軽く予約しておくだけ。
    (void)m_Watchers.TryReserve(4);
    (void)m_WatchedPaths.TryReserve(4);
    (void)m_Callbacks.TryReserve(4);
    (void)m_PendingEvents.TryReserve(8);
    (void)m_EventPaths.TryReserve(8);
    if (!m_Dispatching) {
        m_AbortDispatch = false;
        m_StopDrainAfterCurrentEvent = false;
    }
}

// OS watcher を閉じ、監視 path / callback / pending event を全クリアする。
void CHotReloadWatcher::Shutdown() noexcept {
    // OS watcher ハンドルを閉じる (TUniquePtr<FWatchEntry> の dtor → Close())。
    // 発行中の ReadDirectoryChangesW は CancelIoEx + CloseHandle で取り消される。
    m_Watchers.Clear();

    // watched paths / callbacks / pending events を全クリア。
    // 監視 path は FString の所有値であり、Clear によって解放される。
    // pending event の file_path は m_EventPaths が所有するので両方クリアして
    // dangling を防ぐ (lockstep)。
    m_WatchedPaths.Clear();
    m_Callbacks.Clear();
    m_PendingEvents.Clear();
    m_EventPaths.Clear();
    m_LastConsumedPath.Clear();
    m_AbortDispatch = true;
    m_StopDrainAfterCurrentEvent = true;
}

// ディレクトリを監視登録し、OS watcher (CreateFileW + 初回 read) を起動する。
void CHotReloadWatcher::WatchDirectory(const char* dir_path, bool recursive) noexcept {
    (void)TryWatchDirectory(dir_path, recursive);
}

EHotReloadResult CHotReloadWatcher::TryWatchDirectory(
    const char* dir_path, bool recursive) noexcept {
    usize path_len = 0u;
    const EHotReloadResult validation = ValidatePath(dir_path, path_len);
    if (validation != EHotReloadResult::Success) {
        return validation;
    }
    const FStringView path_view(dir_path, path_len);
    // WatchFile entry は filter 登録にすぎないため、同じ path を後から native
    // directory watcher へ昇格する処理を妨げてはならない。
    for (usize i = 0; i < m_Watchers.Size(); ++i) {
        const FWatchEntry* watcher = m_Watchers[i].Get();
        if (watcher != nullptr && watcher->m_Path == path_view) {
            return EHotReloadResult::AlreadyRegistered;
        }
    }
    bool path_already_listed = false;
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (m_WatchedPaths[i] == path_view) {
            path_already_listed = true;
            break;
        }
    }
    if ((!path_already_listed &&
         m_WatchedPaths.Size() >= kMaxWatchedPaths) ||
        m_Watchers.Size() >= kMaxDirectoryWatches) {
        return EHotReloadResult::LimitExceeded;
    }

    FString owned_path;
    FString listed_path;
    if (!owned_path.TryAppend(path_view) ||
        (!path_already_listed && !listed_path.TryAppend(path_view)) ||
        (!path_already_listed &&
         !m_WatchedPaths.TryReserve(m_WatchedPaths.Size() + 1u)) ||
        !m_Watchers.TryReserve(m_Watchers.Size() + 1u)) {
        return EHotReloadResult::OutOfMemory;
    }

    // ---- OS watcher を起動 ----------------------------------------------
    // UTF-8 path を UTF-16 に変換し、ディレクトリ HANDLE を OVERLAPPED で開く。
    wchar_t wpath[kMaxPathBytes + 1u] = {};
    if (!Utf8ToUtf16(dir_path, path_len, wpath,
                     static_cast<int>(sizeof(wpath) / sizeof(wpath[0])))) {
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: path conversion failed (%s)", dir_path);
        return EHotReloadResult::InvalidUtf8;
    }

    // ディレクトリを開くには FILE_FLAG_BACKUP_SEMANTICS が必須。
    // 非同期 poll のため FILE_FLAG_OVERLAPPED。共有は全許可 (他者の read/write/
    // rename/delete を妨げない — watcher なので当然)。
    HANDLE dir = ::CreateFileW(
        wpath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: CreateFileW failed (%s, err=%lu)",
                     dir_path, static_cast<unsigned long>(err));
        return EHotReloadResult::OsError;
    }

    auto entry = MakeUnique<FWatchEntry>();
    if (!entry) {
        ::CloseHandle(dir);
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: WatchEntry alloc failed (%s)", dir_path);
        return EHotReloadResult::OutOfMemory;
    }
    entry->m_Path      = Move(owned_path);
    entry->m_Dir       = dir;
    entry->m_Recursive = recursive;

    // 最初の ReadDirectoryChangesW を発行 (以後 Tick で完了 → 再発行を繰り返す)。
    if (!entry->IssueRead()) {
        // IssueRead 内で warn 済み。HANDLE は entry の dtor が閉じる。
        return EHotReloadResult::OsError;
    }

    if (!m_Watchers.TryPushBack(Move(entry))) {
        return EHotReloadResult::OutOfMemory;
    }
    if (!path_already_listed &&
        !m_WatchedPaths.TryPushBack(Move(listed_path))) {
        m_Watchers.PopBack();
        return EHotReloadResult::OutOfMemory;
    }
    return EHotReloadResult::Success;
}

// ファイル path を監視 path リストに登録する (個別の OS watcher は持たない)。
void CHotReloadWatcher::WatchFile(const char* file_path) noexcept {
    (void)TryWatchFile(file_path);
}

EHotReloadResult CHotReloadWatcher::TryWatchFile(const char* file_path) noexcept {
    usize path_len = 0u;
    const EHotReloadResult validation = ValidatePath(file_path, path_len);
    if (validation != EHotReloadResult::Success) {
        return validation;
    }
    const FStringView path_view(file_path, path_len);
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (m_WatchedPaths[i] == path_view) {
            return EHotReloadResult::AlreadyRegistered;
        }
    }
    if (m_WatchedPaths.Size() >= kMaxWatchedPaths) {
        return EHotReloadResult::LimitExceeded;
    }
    FString owned_path;
    if (!owned_path.TryAppend(path_view) ||
        !m_WatchedPaths.TryPushBack(Move(owned_path))) {
        return EHotReloadResult::OutOfMemory;
    }
    return EHotReloadResult::Success;
}

// path に対応する OS watcher と監視 path 登録を除去する (未登録は no-op)。
void CHotReloadWatcher::Unwatch(const char* path) noexcept {
    usize path_len = 0u;
    if (ValidatePath(path, path_len) != EHotReloadResult::Success) {
        return;
    }

    // 対応する OS watcher があれば HANDLE を閉じて除去 (TUniquePtr dtor → Close)。
    // m_Watchers は m_WatchedPaths とは別配列 (WatchFile 分は entry を持たない /
    // 起動失敗分も無い) なので path 文字列で照合する。
    for (usize i = 0; i < m_Watchers.Size(); ++i) {
        const FWatchEntry* e = m_Watchers[i].Get();
        if (e != nullptr && e->m_Path == FStringView(path, path_len)) {
            m_Watchers.RemoveAtSwap(i);
            break;
        }
    }

    // 完全一致 1 件を削除。順序は保証しないので swap-remove (RemoveAtSwap)。
    // watched paths は描画レイアウト等の順序依存がないため OK。
    const usize n = m_WatchedPaths.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_WatchedPaths[i] == FStringView(path, path_len)) {
            m_WatchedPaths.RemoveAtSwap(i);
            return;
        }
    }
    // 未登録の Unwatch は no-op (呼び出し側のライフサイクルミスを致命化しない)。
}

// (cb, user) ペアを重複なしで callback リストに登録する。
void CHotReloadWatcher::RegisterCallback(
    FHotReloadCallback cb, void* user) noexcept {
    (void)TryRegisterCallback(cb, user);
}

EHotReloadResult CHotReloadWatcher::TryRegisterCallback(
    FHotReloadCallback cb, void* user) noexcept {
    if (cb == nullptr) {
        return EHotReloadResult::InvalidArgument;
    }

    // (cb, user) ペア重複は no-op。誤って二重 register しても二重 dispatch を防ぐ。
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            return EHotReloadResult::AlreadyRegistered;
        }
    }
    if (m_Callbacks.Size() >= kMaxCallbacks) {
        return EHotReloadResult::LimitExceeded;
    }
    FCallbackEntry e{};
    e.cb   = cb;
    e.user = user;
    return m_Callbacks.TryPushBack(e)
        ? EHotReloadResult::Success
        : EHotReloadResult::OutOfMemory;
}

EHotReloadResult CHotReloadWatcher::UnregisterCallback(
    FHotReloadCallback cb, void* user) noexcept {
    if (cb == nullptr) {
        return EHotReloadResult::InvalidArgument;
    }
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            m_Callbacks.RemoveAtSwap(i);
            return EHotReloadResult::Success;
        }
    }
    return EHotReloadResult::NotRegistered;
}

EHotReloadResult CHotReloadWatcher::TrySetDebounceSeconds(f32 seconds) noexcept {
    if (!std::isfinite(seconds) || seconds < 0.0f ||
        seconds > kMaxDebounceSeconds) {
        return EHotReloadResult::InvalidArgument;
    }
    m_DebounceSeconds = seconds;
    return EHotReloadResult::Success;
}

f32 CHotReloadWatcher::DebounceSeconds() const noexcept {
    return m_DebounceSeconds;
}

EHotReloadResult CHotReloadWatcher::TryEnqueueEvent(
    const char* file_path, u64 modified_timestamp, bool removed) noexcept {
    usize path_len = 0u;
    const EHotReloadResult validation = ValidatePath(file_path, path_len);
    if (validation != EHotReloadResult::Success) {
        RecordDiagnosticFailure(validation, true, false);
        return validation;
    }
    FString owned_path;
    if (!owned_path.TryAppend(FStringView(file_path, path_len))) {
        RecordDiagnosticFailure(EHotReloadResult::OutOfMemory, true, true);
        return EHotReloadResult::OutOfMemory;
    }
    const EHotReloadResult result =
        TryQueueEvent(Move(owned_path), modified_timestamp, removed);
    if (result != EHotReloadResult::Success) {
        const bool notification_lost =
            result == EHotReloadResult::LimitExceeded ||
            result == EHotReloadResult::OutOfMemory;
        RecordDiagnosticFailure(result, true, notification_lost);
    }
    return result;
}

EHotReloadResult CHotReloadWatcher::TryQueueEvent(
    FString&& path, u64 timestamp, bool removed) noexcept {
    if (path.IsEmpty() || path.Size() > kMaxPathBytes) {
        return EHotReloadResult::InvalidArgument;
    }

    u64 debounce_ms = 0u;
    if (m_DebounceSeconds > 0.0f) {
        debounce_ms =
            static_cast<u64>(m_DebounceSeconds * 1000.0f);
        // event timestamp は整数ミリ秒精度。厳密な 0 だけが debounce を無効にする
        // 公開契約を守るため、正の 1ms 未満の間隔も 1 tick として扱う。
        if (debounce_ms == 0u) {
            debounce_ms = 1u;
        }
    }
    if (debounce_ms > 0u) {
        // 完全な event/path pair の末尾から検索する。古い burst が queue に残っていても
        // 正しい debounce 候補を選び、内部 lockstep 不変条件が崩れた場合も範囲内を保つ。
        const usize pair_count =
            m_EventPaths.Size() < m_PendingEvents.Size()
                ? m_EventPaths.Size()
                : m_PendingEvents.Size();
        for (usize i = pair_count; i-- > 0u;) {
            if (m_EventPaths[i] != path) {
                continue;
            }
            FHotReloadEvent& existing = m_PendingEvents[i];
            if (timestamp >= existing.modified_timestamp &&
                timestamp - existing.modified_timestamp <= debounce_ms) {
                existing.modified_timestamp = timestamp;
                existing.removed = removed;
                hot_reload_detail::SaturatingIncrement(
                    m_Diagnostics.coalesced_event_count);
                return EHotReloadResult::Success;
            }
            break;
        }
    }

    if (m_PendingEvents.Size() >= kMaxPendingEvents) {
        return EHotReloadResult::LimitExceeded;
    }
    const usize required = m_PendingEvents.Size() + 1u;
    if (!m_PendingEvents.TryReserve(required) ||
        !m_EventPaths.TryReserve(required)) {
        return EHotReloadResult::OutOfMemory;
    }

    FHotReloadEvent event{};
    event.modified_timestamp = timestamp;
    event.removed = removed;
    if (!m_EventPaths.TryPushBack(Move(path))) {
        return EHotReloadResult::OutOfMemory;
    }
    if (!m_PendingEvents.TryPushBack(event)) {
        m_EventPaths.PopBack();
        return EHotReloadResult::OutOfMemory;
    }
    hot_reload_detail::SaturatingIncrement(
        m_Diagnostics.enqueued_event_count);
    return EHotReloadResult::Success;
}

EHotReloadResult CHotReloadWatcher::ProcessNativeNotificationBuffer(
    const void* bytes, usize byte_count, FStringView directory_path,
    bool force_conversion_out_of_memory) noexcept {
    EHotReloadResult aggregate = EHotReloadResult::Success;
    const auto record_failure =
        [this, &aggregate](
            EHotReloadResult result, bool rejected_event) noexcept {
            RecordDiagnosticFailure(result, rejected_event, true);
            AccumulateHotReloadResult(result, aggregate);
        };

    if (bytes == nullptr || byte_count == 0u ||
        byte_count > FWatchEntry::kBufferBytes) {
        record_failure(EHotReloadResult::OsError, true);
        return aggregate;
    }

    const auto* base = static_cast<const u8*>(bytes);
    usize off = 0u;
    while (off < byte_count) {
        constexpr usize kRecordPrefix =
            offsetof(FILE_NOTIFY_INFORMATION, FileName);
        const usize remaining = byte_count - off;
        if (remaining < kRecordPrefix) {
            record_failure(EHotReloadResult::OsError, true);
            break;
        }

        const auto* fni =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);
        const usize name_bytes = static_cast<usize>(fni->FileNameLength);
        if (!hot_reload_detail::IsValidNativeNotificationFields(
                static_cast<u32>(fni->Action), name_bytes) ||
            name_bytes > remaining - kRecordPrefix) {
            // 空 name、未知 action、範囲外 filename は正確な変更へ対応付けられない。
            record_failure(EHotReloadResult::OsError, true);
            break;
        }

        const int wlen =
            static_cast<int>(fni->FileNameLength / sizeof(WCHAR));
        FString full;
        EHotReloadResult path_result = ConvertNativeUtf16ToUtf8(
            fni->FileName, wlen, full,
            force_conversion_out_of_memory);
        if (path_result == EHotReloadResult::Success) {
            path_result = JoinNativePath(directory_path, full);
        }
        if (path_result != EHotReloadResult::Success) {
            // OOM は診断上区別するが、いずれも有効な native event を失うため
            // authoritative rescan が必要。
            record_failure(path_result, true);
        } else {
            const EHotReloadResult queued = TryQueueEvent(
                Move(full), static_cast<u64>(::GetTickCount64()),
                ActionIsRemoval(fni->Action));
            if (queued != EHotReloadResult::Success) {
                record_failure(queued, true);
            }
        }

        if (fni->NextEntryOffset == 0u) {
            break;
        }
        const usize next = static_cast<usize>(fni->NextEntryOffset);
        if ((next % alignof(DWORD)) != 0u ||
            next < kRecordPrefix + name_bytes ||
            next >= remaining) {
            // 非 zero offset は buffer 内に実在する次 record を指す必要がある。
            // buffer 末尾ちょうどは「次 record なし」なので malformed。
            record_failure(EHotReloadResult::OsError, true);
            break;
        }
        off += next;
    }
    return aggregate;
}

// 各 watcher の完了を poll して event を積み、pending を callback へ FIFO で dispatch する。
void CHotReloadWatcher::Tick(f32 dt) noexcept {
    (void)TryTick(dt);
}

EHotReloadResult CHotReloadWatcher::TryTick(f32 dt) noexcept {
    if (!std::isfinite(dt) || dt < 0.0f) {
        RecordDiagnosticFailure(
            EHotReloadResult::InvalidArgument, false, false);
        return EHotReloadResult::InvalidArgument;
    }
    if (m_Dispatching) {
        RecordDiagnosticFailure(
            EHotReloadResult::ReentrantCall, false, false);
        return EHotReloadResult::ReentrantCall;
    }
    m_AbortDispatch = false;
    m_StopDrainAfterCurrentEvent = false;
    EHotReloadResult tick_result = EHotReloadResult::Success;
    const auto record_result =
        [this, &tick_result](
            EHotReloadResult candidate, bool rejected_event,
            bool notification_lost) noexcept {
            RecordDiagnosticFailure(
                candidate, rejected_event, notification_lost);
            AccumulateHotReloadResult(candidate, tick_result);
        };

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
    // unit test build だけで、次の native 完了状態または synthetic buffer を
    // 実際の TryTick 集約・診断経路へ 1 回通す。
    using EInjectedFault = internal::EHotReloadNativeFaultForTesting;
    const EInjectedFault injected_fault = g_InjectedNativeState.fault;
    g_InjectedNativeState.fault = EInjectedFault::None;
    switch (injected_fault) {
    case EInjectedFault::None:
        break;
    case EInjectedFault::NativeOverflow:
        AccumulateHotReloadResult(
            HandleNativeOverflowCompletion(true), tick_result);
        break;
    case EInjectedFault::RearmFailure:
        AccumulateHotReloadResult(
            HandleNativeOverflowCompletion(false), tick_result);
        break;
    case EInjectedFault::SyntheticBuffer:
    case EInjectedFault::SyntheticBufferConversionOutOfMemory: {
        const EHotReloadResult synthetic_result =
            ProcessNativeNotificationBuffer(
                g_InjectedNativeState.bytes,
                g_InjectedNativeState.byte_count,
                FStringView{},
                injected_fault ==
                    EInjectedFault::SyntheticBufferConversionOutOfMemory);
        AccumulateHotReloadResult(synthetic_result, tick_result);
        break;
    }
    }
#endif

    // ---- 各 watcher の完了をノンブロッキングで poll ----------------------
    // GetOverlappedResult(bWait=FALSE) で ReadDirectoryChangesW の完了を確認。
    // 完了していれば FILE_NOTIFY_INFORMATION を走査して event を積み、
    // 同じ HANDLE に対し read を再発行する (継続監視)。
    for (usize wi = 0; wi < m_Watchers.Size(); ++wi) {
        FWatchEntry* e = m_Watchers[wi].Get();
        if (e == nullptr || e->m_Dir == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (!e->m_ReadPending) {
            // 前回 read 発行に失敗していた場合はここで再試行する。
            if (!e->IssueRead()) {
                record_result(EHotReloadResult::OsError, false, true);
            }
            continue;
        }

        DWORD bytes = 0;
        if (::GetOverlappedResult(e->m_Dir, &e->m_Overlapped, &bytes, FALSE) == FALSE) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_INCOMPLETE) {
                continue;  // まだ完了していない (正常)
            }
            if (err == ERROR_NOTIFY_ENUM_DIR) {
                // Windows は通知 buffer の欠落を zero-byte completion または
                // この status のどちらでも報告できる。
                e->m_ReadPending = false;
                AccumulateHotReloadResult(
                    HandleNativeOverflowCompletion(e->IssueRead()),
                    tick_result);
                continue;
            }
            // 取り消し / その他エラー: read pending を畳んで次フレーム再発行を試みる。
            ACS_LOG_WARN("HotReloadWatcher: GetOverlappedResult failed (err=%lu)",
                         static_cast<unsigned long>(err));
            e->m_ReadPending = false;
            (void)e->IssueRead();
            record_result(EHotReloadResult::OsError, false, true);
            continue;
        }
        e->m_ReadPending = false;

        // bytes == 0 はバッファ溢れ (変更が多すぎて取りこぼし)。監視を
        // 再発行しつつ caller に authoritative rescan を要求する。
        if (bytes == 0) {
            AccumulateHotReloadResult(
                HandleNativeOverflowCompletion(e->IssueRead()),
                tick_result);
            continue;
        }

        // ---- FILE_NOTIFY_INFORMATION のリンク列を走査 -------------------
        // parser 自身が拒否・欠落診断を記録するため、ここでは返却値だけを集約する。
        const EHotReloadResult parse_result =
            ProcessNativeNotificationBuffer(
                e->m_Buffer, static_cast<usize>(bytes), e->m_Path.View());
        AccumulateHotReloadResult(parse_result, tick_result);

        // 次の変更を拾うため read を再発行 (one-shot の継続化)。
        if (!e->IssueRead()) {
            record_result(EHotReloadResult::OsError, false, true);
        }
    }

    // ---- 登録済み callback へ dispatch ----------------------------------
    // pending を FIFO で drain しながら callback を呼ぶ。Consume と同じ FIFO 規約。
    // file_path は呼び出し直前に m_EventPaths から解決して「常に現行の安定アドレス」
    // を渡す (TArray 再確保で SSO 文字列のアドレスが動いても安全)。
    if (m_PendingEvents.Size() == 0 || m_Callbacks.Size() == 0) {
        return tick_result;
    }

    TArray<FCallbackEntry> callback_snapshot;
    if (!callback_snapshot.TryReserve(m_Callbacks.Size())) {
        record_result(EHotReloadResult::OutOfMemory, false, false);
        return tick_result;
    }
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (!callback_snapshot.TryPushBack(m_Callbacks[i])) {
            record_result(EHotReloadResult::OutOfMemory, false, false);
            return tick_result;
        }
    }

    m_Dispatching = true;
    usize dispatch_remaining = m_PendingEvents.Size();
    while (dispatch_remaining > 0u &&
           m_PendingEvents.Size() > 0u &&
           !m_AbortDispatch &&
           !m_StopDrainAfterCurrentEvent) {
        FHotReloadEvent ev = m_PendingEvents[0];
        FString event_path =
            (m_EventPaths.Size() > 0) ? Move(m_EventPaths[0]) : FString{};
        RemoveFrontEventPair();
        --dispatch_remaining;
        hot_reload_detail::SaturatingIncrement(
            m_Diagnostics.dispatched_event_count);
        ev.file_path = event_path.Data();

        for (usize ci = 0; ci < callback_snapshot.Size() && !m_AbortDispatch; ++ci) {
            const FCallbackEntry& c = callback_snapshot[ci];
            bool still_registered = false;
            for (usize current = 0; current < m_Callbacks.Size(); ++current) {
                if (m_Callbacks[current].cb == c.cb &&
                    m_Callbacks[current].user == c.user) {
                    still_registered = true;
                    break;
                }
            }
            if (still_registered && c.cb != nullptr) {
                c.cb(c.user, ev);
            }
        }
    }
    m_Dispatching = false;
    return tick_result;
}

// 監視登録された path の数を返す。
u32 CHotReloadWatcher::WatchedCount() const noexcept {
    return static_cast<u32>(m_WatchedPaths.Size());
}

// 未消費の pending event 数を返す。
u32 CHotReloadWatcher::PendingEventCount() const noexcept {
    return static_cast<u32>(m_PendingEvents.Size());
}

// 診断値を allocation-free の値コピーとして取得する。
FHotReloadDiagnostics CHotReloadWatcher::CaptureDiagnostics() const noexcept {
    return m_Diagnostics;
}

// 診断値だけを確認済み状態へ戻し、監視・callback・event queue は維持する。
void CHotReloadWatcher::ClearDiagnostics() noexcept {
    m_Diagnostics = FHotReloadDiagnostics{};
}

// pending event の先頭 1 件を FIFO で取り出して除去する (空なら false)。
bool CHotReloadWatcher::ConsumeNextEvent(FHotReloadEvent& out) noexcept {
    if (m_PendingEvents.Size() == 0 || m_EventPaths.Size() == 0) {
        return false;  // 空なら out は触らず false
    }
    if (m_Dispatching) {
        // callback が開始時 queue を手動消費した場合も、新規 enqueue が空いた
        // dispatch 枠へ入り込まないよう現在 event 後の外側 drain を止める。
        m_StopDrainAfterCurrentEvent = true;
    }

    // FIFO 順で先頭を取り出す。file_path は cache せず、対応する所有文字列
    // (m_EventPaths[0]) の現行アドレスから解決する。返した out.file_path は
    // 「次に Consume / ClearEvents / Shutdown するまで」valid。
    const FHotReloadEvent event = m_PendingEvents[0];
    m_LastConsumedPath = Move(m_EventPaths[0]);

    // 先頭を物理削除して shift-left (m_PendingEvents / m_EventPaths を lockstep)。
    // pending size は <= 数十想定なので shift コストは実用上無視できる。
    // (swap-remove は順序を壊すため不採用 — hot reload は「最初に届いた変更を
    //  最初に処理する」のが直感的)
    RemoveFrontEventPair();
    out = event;
    out.file_path = m_LastConsumedPath.Data();
    return true;
}

// pending event 先頭 1 件を m_PendingEvents / m_EventPaths から lockstep で shift 除去する。
void CHotReloadWatcher::RemoveFrontEventPair() noexcept {
    if (m_PendingEvents.Size() > 0) {
        for (usize i = 1; i < m_PendingEvents.Size(); ++i) {
            m_PendingEvents[i - 1] = m_PendingEvents[i];
        }
        m_PendingEvents.PopBack();
    }
    if (m_EventPaths.Size() > 0) {
        for (usize i = 1; i < m_EventPaths.Size(); ++i) {
            m_EventPaths[i - 1] = Move(m_EventPaths[i]);
        }
        m_EventPaths.PopBack();
    }
}

// pending event と所有 path 文字列を lockstep で全クリアする。
void CHotReloadWatcher::ClearEvents() noexcept {
    // callback 内で ClearEvents 後に event を enqueue しても、開始時 event の残り枠で
    // 同じ TryTick に配信してはならない。現在 event の他 callback は完走させ、
    // 次 event へ進む外側 drain だけを停止する。
    if (m_Dispatching) {
        m_StopDrainAfterCurrentEvent = true;
    }
    // pending events と所有 path 文字列を lockstep で全クリア。
    m_PendingEvents.Clear();
    m_EventPaths.Clear();
    m_LastConsumedPath.Clear();
}

#else // ACS_GAME_SHIPPING

// Ship build (ACS_GAME_SHIPPING) では全 method を no-op にする。シンボル定義は残し、
// 呼び出し側コードが #ifdef だらけにならないようにする。戻り値は安全な既定値 (0 / false)。
// 診断を含む instance storage を追加しない契約も compile time で固定する。
static_assert(
    sizeof(CHotReloadWatcher) == 1u,
    "Shipping FHotReloadWatcher must remain storage-free");
constexpr FHotReloadDiagnostics kShippingZeroDiagnostics{};
static_assert(
    kShippingZeroDiagnostics.enqueued_event_count == 0u &&
    kShippingZeroDiagnostics.coalesced_event_count == 0u &&
    kShippingZeroDiagnostics.dispatched_event_count == 0u &&
    kShippingZeroDiagnostics.rejected_event_count == 0u &&
    kShippingZeroDiagnostics.loss_incident_count == 0u &&
    kShippingZeroDiagnostics.last_failure == EHotReloadResult::Success &&
    !kShippingZeroDiagnostics.authoritative_rescan_required,
    "Shipping hot reload diagnostics must remain a deterministic zero snapshot");

// ship build の既定コンストラクタ (no-op)。
CHotReloadWatcher::CHotReloadWatcher() noexcept = default;

// ship build のデストラクタ (no-op)。
CHotReloadWatcher::~CHotReloadWatcher() noexcept {}

// ship build では監視を行わない (no-op)。
void CHotReloadWatcher::Init() noexcept {}

// ship build では解放するものが無い (no-op)。
void CHotReloadWatcher::Shutdown() noexcept {}

// ship build ではディレクトリ監視を行わない (no-op)。
void CHotReloadWatcher::WatchDirectory(const char*, bool) noexcept {}

EHotReloadResult CHotReloadWatcher::TryWatchDirectory(
    const char* dir_path, bool) noexcept {
    return (dir_path != nullptr && dir_path[0] != '\0')
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

// ship build ではファイル監視を行わない (no-op)。
void CHotReloadWatcher::WatchFile(const char*) noexcept {}

EHotReloadResult CHotReloadWatcher::TryWatchFile(const char* file_path) noexcept {
    return (file_path != nullptr && file_path[0] != '\0')
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

// ship build では監視解除するものが無い (no-op)。
void CHotReloadWatcher::Unwatch(const char*) noexcept {}

// ship build では callback を登録しない (no-op)。
void CHotReloadWatcher::RegisterCallback(
    FHotReloadCallback, void*) noexcept {}

EHotReloadResult CHotReloadWatcher::TryRegisterCallback(
    FHotReloadCallback cb, void*) noexcept {
    return cb != nullptr
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

EHotReloadResult CHotReloadWatcher::UnregisterCallback(
    FHotReloadCallback cb, void*) noexcept {
    return cb != nullptr
        ? EHotReloadResult::NotRegistered
        : EHotReloadResult::InvalidArgument;
}

EHotReloadResult CHotReloadWatcher::TrySetDebounceSeconds(f32 seconds) noexcept {
    return (seconds >= 0.0f && seconds <= kMaxDebounceSeconds)
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

f32 CHotReloadWatcher::DebounceSeconds() const noexcept { return 0.0f; }

EHotReloadResult CHotReloadWatcher::TryEnqueueEvent(
    const char* file_path, u64, bool) noexcept {
    return (file_path != nullptr && file_path[0] != '\0')
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

// ship build では駆動しない (no-op)。
void CHotReloadWatcher::Tick(f32) noexcept {}

EHotReloadResult CHotReloadWatcher::TryTick(f32 dt) noexcept {
    return (dt >= 0.0f && (dt - dt) == 0.0f)
        ? EHotReloadResult::Success
        : EHotReloadResult::InvalidArgument;
}

// ship build の監視 path 数を返す。常に 0。
u32  CHotReloadWatcher::WatchedCount() const noexcept { return 0; }

// ship build の pending event 数を返す。常に 0。
u32  CHotReloadWatcher::PendingEventCount() const noexcept { return 0; }

// ship build は診断 storage を持たず、常に決定的なゼロ snapshot を返す。
FHotReloadDiagnostics CHotReloadWatcher::CaptureDiagnostics() const noexcept {
    return FHotReloadDiagnostics{};
}

// ship build には clear 対象の診断 storage が無い。
void CHotReloadWatcher::ClearDiagnostics() noexcept {}

// ship build の event 取り出しは常に失敗し、out には触れず false を返す。
bool CHotReloadWatcher::ConsumeNextEvent(FHotReloadEvent&) noexcept { return false; }

// ship build では event を持たない (no-op)。
void CHotReloadWatcher::ClearEvents() noexcept {}

#endif // ACS_GAME_SHIPPING

} // namespace acs::game
