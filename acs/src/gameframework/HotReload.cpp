// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FHotReload 実装 (dev build = 実 FS watcher)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・watched paths / callbacks / pending events の TArray に加え、監視ディレクトリ
//     ごとの OS watcher 状態 (WatchEntry) を持つレジストリ。
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
#include <cstring>                 // strcmp
#include "foundation/Platform.h"   // <windows.h> (ReadDirectoryChangesW 等)
#include "foundation/Move.h"       // Move
#include "memory/UniquePtr.h"      // MakeUnique
#endif

namespace acs::game {

#ifndef ACS_GAME_SHIPPING

/**
 * 監視ディレクトリ 1 件あたりの ReadDirectoryChangesW 状態。
 *
 * @details
 * HANDLE + OVERLAPPED + 受信バッファ + recursive フラグを束ねる。OVERLAPPED と m_Buffer
 * のアドレスは I/O 発行から完了通知まで安定している必要があるため、この struct は
 * TUniquePtr で個別 heap 確保される (ヘッダ参照)。
 */
struct WatchEntry {
    /**
     * ReadDirectoryChangesW の受信バッファのバイト数。
     *
     * @details
     * FILE_NOTIFY_INFORMATION は DWORD 整列が要求されるため u32 配列で確保してアラインを
     * 保証する。32KiB あれば 1 フレーム分の連続変更には十分 (溢れたら次の read で取り直す)。
     */
    static constexpr DWORD kBufferBytes = 32u * 1024u;

    /** 監視 dir の UTF-8 path (借用。Unwatch 照合用)。 */
    const char* m_Path      = nullptr;

    /** CreateFileW で開いたディレクトリ HANDLE。 */
    HANDLE     m_Dir       = INVALID_HANDLE_VALUE;

    /** サブディレクトリも監視するか。 */
    bool       m_Recursive = true;

    /** ReadDirectoryChangesW 発行中か。 */
    bool       m_ReadPending = false;

    /** 非同期 I/O 状態 (アドレス固定が必須)。 */
    OVERLAPPED m_Overlapped{};

    /** DWORD 整列された受信バッファ。 */
    u32        m_Buffer[kBufferBytes / sizeof(u32)] = {};

    /** 空状態で構築する (m_Dir は invalid、HANDLE は未確保)。 */
    WatchEntry() noexcept = default;

    /** 破棄する (Close で I/O 取り消し + HANDLE クローズ)。 */
    ~WatchEntry() noexcept {
        Close();
    }

    /** コピー禁止 (HANDLE / OVERLAPPED の所有を曖昧にしないため)。 */
    WatchEntry(const WatchEntry&)            = delete;

    /** コピー代入も禁止。 */
    WatchEntry& operator=(const WatchEntry&) = delete;

    /** ムーブ禁止。 */
    WatchEntry(WatchEntry&&)                 = delete;

    /** ムーブ代入も禁止。 */
    WatchEntry& operator=(WatchEntry&&)      = delete;

    /** 発行中の I/O を取り消し、HANDLE を閉じる (多重呼び出し安全)。 */
    void Close() noexcept {
        if (m_Dir != INVALID_HANDLE_VALUE) {
            // 発行中の ReadDirectoryChangesW を取り消す (CloseHandle でも暗黙に
            // 取り消されるが、明示しておく)。
            if (m_ReadPending) {
                ::CancelIoEx(m_Dir, &m_Overlapped);
                m_ReadPending = false;
            }
            ::CloseHandle(m_Dir);
            m_Dir = INVALID_HANDLE_VALUE;
        }
    }

    /**
     * ReadDirectoryChangesW を 1 回発行する (one-shot)。
     *
     * @details OVERLAPPED を毎回リセットし、completion routine を使わず poll 方式で待つ。
     * @return 発行に成功して m_ReadPending=true になれば true、失敗なら false。
     */
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

namespace {

/**
 * FILE_ACTION_* を「消えた」フラグへマップする。
 *
 * @details 削除 (FILE_ACTION_REMOVED) と rename-from (RENAMED_OLD_NAME) を removed 扱いにする。
 * @param action FILE_NOTIFY_INFORMATION の Action 値。
 * @return 削除系のアクションなら true。
 */
bool ActionIsRemoval(DWORD action) noexcept {
    return action == FILE_ACTION_REMOVED ||
           action == FILE_ACTION_RENAMED_OLD_NAME;
}

/**
 * WCHAR (UTF-16, 非 NUL 終端 + 文字数指定) を FString (UTF-8) へ変換する。
 *
 * @details 既存内容は上書き (Clear してから Append)。変換失敗時は空のままにする。
 * @param w UTF-16 文字列の先頭 (NUL 終端不要)。
 * @param wlen w の文字数 (WCHAR 単位)。
 * @param out 変換結果の格納先 (UTF-8)。
 */
void Utf16ToUtf8(const wchar_t* w, int wlen, FString& out) noexcept {
    out.Clear();
    if (w == nullptr || wlen <= 0) {
        return;
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return;
    }
    out.Reserve(static_cast<usize>(need) + 1);
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
            return;
        }
    }
    const int got = ::WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                                          buf, need, nullptr, nullptr);
    if (got > 0) {
        out.Append(FStringView(buf, static_cast<usize>(got)));
    }
    if (alloc != nullptr) {
        alloc->Free(buf);
    }
}

/**
 * UTF-8 の path を UTF-16 へ変換し、NUL 終端で out_w に書く。
 *
 * @param u8 入力 UTF-8 文字列 (NUL 終端)。
 * @param out_w 出力 UTF-16 バッファ。
 * @param out_cap out_w の要素数 (WCHAR 単位)。
 * @return 変換成功なら true、バッファ不足 / 変換失敗なら false。
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
 * dir path に "/" 区切りで相対 path を連結して絶対 path に復元する。
 *
 * @details ReadDirectoryChangesW の filename は dir 相対なので、これで絶対 path に戻す。
 * @param dir 監視 dir の UTF-8 path (空 / nullptr なら rel_inout をそのまま使う)。
 * @param rel_inout 入力は dir 相対 path、出力は連結後の絶対 path (UTF-8)。
 */
void JoinPath(const char* dir, FString& rel_inout) noexcept {
    FString joined;
    if (dir != nullptr && dir[0] != '\0') {
        usize dir_len = 0;
        while (dir[dir_len]) ++dir_len;
        joined.Append(FStringView(dir, dir_len));
        const usize n = joined.Size();
        const char last = (n > 0) ? joined[n - 1] : '\0';
        if (last != '/' && last != '\\') {
            joined.Append('/');
        }
    }
    joined.Append(rel_inout.View());
    rel_inout = Move(joined);
}

} // namespace

/** デストラクタ (WatchEntry が完全型になる本 TU で実体化し Shutdown を呼ぶ)。 */
HotReloadWatcher::~HotReloadWatcher() noexcept {
    Shutdown();
}

/** 内部バッファを軽く予約する (OS watcher の起動は WatchDirectory が担う)。 */
void HotReloadWatcher::Init() noexcept {
    // 既に WatchDirectory 済みなら何もしない (多重 Init 安全)。実際の OS watcher
    // 起動 (CreateFileW + ReadDirectoryChangesW) は WatchDirectory が担う。
    // ここではバッファを軽く予約しておくだけ。
    m_Watchers.Reserve(4);
    m_PendingEvents.Reserve(8);
    m_EventPaths.Reserve(8);
}

/** OS watcher を閉じ、監視 path / callback / pending event を全クリアする。 */
void HotReloadWatcher::Shutdown() noexcept {
    // OS watcher ハンドルを閉じる (TUniquePtr<WatchEntry> の dtor → Close())。
    // 発行中の ReadDirectoryChangesW は CancelIoEx + CloseHandle で取り消される。
    m_Watchers.Clear();

    // watched paths / callbacks / pending events を全クリア。
    // path 文字列自体は caller 所有 (借用) なので Free しない。
    // pending event の file_path は m_EventPaths が所有するので両方クリアして
    // dangling を防ぐ (lockstep)。
    m_WatchedPaths.Clear();
    m_Callbacks.Clear();
    m_PendingEvents.Clear();
    m_EventPaths.Clear();
}

/** ディレクトリを監視登録し、OS watcher (CreateFileW + 初回 read) を起動する。 */
void HotReloadWatcher::WatchDirectory(const char* dir_path, bool recursive) noexcept {
    if (dir_path == nullptr || dir_path[0] == '\0') {
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: null/empty path (ignored)");
        return;
    }

    // 重複登録は no-op (二重 dispatch / Unwatch の対称性破壊を防ぐ)。
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (std::strcmp(m_WatchedPaths[i], dir_path) == 0) {
            return;
        }
    }
    m_WatchedPaths.PushBack(dir_path);

    // ---- OS watcher を起動 ----------------------------------------------
    // UTF-8 path を UTF-16 に変換し、ディレクトリ HANDLE を OVERLAPPED で開く。
    wchar_t wpath[MAX_PATH * 2];
    if (!Utf8ToUtf16(dir_path, wpath, static_cast<int>(sizeof(wpath) / sizeof(wpath[0])))) {
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: path conversion failed (%s)", dir_path);
        return;  // watched paths への登録は維持 (列挙には出す)、OS watcher のみ未起動
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
        return;
    }

    auto entry = MakeUnique<WatchEntry>();
    if (!entry) {
        ::CloseHandle(dir);
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: WatchEntry alloc failed (%s)", dir_path);
        return;
    }
    entry->m_Path      = dir_path;  // caller 所有 (借用)、m_WatchedPaths と同じ寿命
    entry->m_Dir       = dir;
    entry->m_Recursive = recursive;

    // 最初の ReadDirectoryChangesW を発行 (以後 Tick で完了 → 再発行を繰り返す)。
    if (!entry->IssueRead()) {
        // IssueRead 内で warn 済み。HANDLE は entry の dtor が閉じる。
        return;
    }

    m_Watchers.PushBack(Move(entry));
}

/** ファイル path を監視 path リストに登録する (個別の OS watcher は持たない)。 */
void HotReloadWatcher::WatchFile(const char* file_path) noexcept {
    if (file_path == nullptr || file_path[0] == '\0') {
        ACS_LOG_WARN("HotReloadWatcher::WatchFile: null/empty path (ignored)");
        return;
    }

    // 重複登録は no-op (WatchDirectory と同じ理由)。
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (std::strcmp(m_WatchedPaths[i], file_path) == 0) {
            return;
        }
    }
    m_WatchedPaths.PushBack(file_path);
}

/** path に対応する OS watcher と監視 path 登録を除去する (未登録は no-op)。 */
void HotReloadWatcher::Unwatch(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return;  // null/empty は no-op (境界条件吸収)
    }

    // 対応する OS watcher があれば HANDLE を閉じて除去 (TUniquePtr dtor → Close)。
    // m_Watchers は m_WatchedPaths とは別配列 (WatchFile 分は entry を持たない /
    // 起動失敗分も無い) なので path 文字列で照合する。
    for (usize i = 0; i < m_Watchers.Size(); ++i) {
        const WatchEntry* e = m_Watchers[i].Get();
        if (e != nullptr && e->m_Path != nullptr && std::strcmp(e->m_Path, path) == 0) {
            m_Watchers.RemoveAtSwap(i);
            break;
        }
    }

    // 完全一致 1 件を削除。順序は保証しないので swap-remove (RemoveAtSwap)。
    // watched paths は描画レイアウト等の順序依存がないため OK。
    const usize n = m_WatchedPaths.Size();
    for (usize i = 0; i < n; ++i) {
        if (std::strcmp(m_WatchedPaths[i], path) == 0) {
            m_WatchedPaths.RemoveAtSwap(i);
            return;
        }
    }
    // 未登録の Unwatch は no-op (呼び出し側のライフサイクルミスを致命化しない)。
}

/** (cb, user) ペアを重複なしで callback リストに登録する。 */
void HotReloadWatcher::RegisterCallback(HotReloadCallback cb, void* user) noexcept {
    if (cb == nullptr) {
        ACS_LOG_WARN("HotReloadWatcher::RegisterCallback: null cb (ignored)");
        return;
    }

    // (cb, user) ペア重複は no-op。誤って二重 register しても二重 dispatch を防ぐ。
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            return;
        }
    }
    CallbackEntry e{};
    e.cb   = cb;
    e.user = user;
    m_Callbacks.PushBack(e);
}

/** 各 watcher の完了を poll して event を積み、pending を callback へ FIFO で dispatch する。 */
void HotReloadWatcher::Tick(f32 dt) noexcept {
    (void)dt;

    // ---- 各 watcher の完了をノンブロッキングで poll ----------------------
    // GetOverlappedResult(bWait=FALSE) で ReadDirectoryChangesW の完了を確認。
    // 完了していれば FILE_NOTIFY_INFORMATION を走査して event を積み、
    // 同じ HANDLE に対し read を再発行する (継続監視)。
    for (usize wi = 0; wi < m_Watchers.Size(); ++wi) {
        WatchEntry* e = m_Watchers[wi].Get();
        if (e == nullptr || e->m_Dir == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (!e->m_ReadPending) {
            // 前回 read 発行に失敗していた場合はここで再試行する。
            e->IssueRead();
            continue;
        }

        DWORD bytes = 0;
        if (::GetOverlappedResult(e->m_Dir, &e->m_Overlapped, &bytes, FALSE) == FALSE) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_INCOMPLETE) {
                continue;  // まだ完了していない (正常)
            }
            // 取り消し / その他エラー: read pending を畳んで次フレーム再発行を試みる。
            ACS_LOG_WARN("HotReloadWatcher: GetOverlappedResult failed (err=%lu)",
                         static_cast<unsigned long>(err));
            e->m_ReadPending = false;
            e->IssueRead();
            continue;
        }
        e->m_ReadPending = false;

        // bytes == 0 はバッファ溢れ (変更が多すぎて取りこぼし)。event は出せない
        // が監視は継続したいので read を再発行するだけ。
        if (bytes == 0) {
            e->IssueRead();
            continue;
        }

        // ---- FILE_NOTIFY_INFORMATION のリンク列を走査 -------------------
        const u8* base = reinterpret_cast<const u8*>(e->m_Buffer);
        usize     off  = 0;
        for (;;) {
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);

            // FileNameLength はバイト数 (WCHAR 単位ではない)。NUL 終端なし。
            const int wlen = static_cast<int>(fni->FileNameLength / sizeof(WCHAR));
            if (wlen > 0) {
                // 相対 filename (UTF-16) を UTF-8 化 → dir と連結して絶対 path に。
                FString full;
                Utf16ToUtf8(fni->FileName, wlen, full);
                if (!full.IsEmpty()) {
                    JoinPath(e->m_Path, full);

                    // lockstep で push。file_path は dispatch / Consume 時に
                    // m_EventPaths から解決するため、ここでは nullptr のまま。
                    HotReloadEvent ev{};
                    ev.file_path         = nullptr;
                    ev.modified_timestamp = static_cast<u64>(::GetTickCount64());
                    ev.removed           = ActionIsRemoval(fni->Action);
                    m_PendingEvents.PushBack(ev);
                    m_EventPaths.PushBack(Move(full));
                }
            }

            if (fni->NextEntryOffset == 0) {
                break;  // リンク末尾
            }
            off += fni->NextEntryOffset;
            if (off >= WatchEntry::kBufferBytes) {
                break;  // 念のための境界保護
            }
        }

        // 次の変更を拾うため read を再発行 (one-shot の継続化)。
        e->IssueRead();
    }

    // ---- 登録済み callback へ dispatch ----------------------------------
    // pending を FIFO で drain しながら callback を呼ぶ。Consume と同じ FIFO 規約。
    // file_path は呼び出し直前に m_EventPaths から解決して「常に現行の安定アドレス」
    // を渡す (TArray 再確保で SSO 文字列のアドレスが動いても安全)。
    while (m_PendingEvents.Size() > 0 && m_Callbacks.Size() > 0) {
        HotReloadEvent ev = m_PendingEvents[0];
        ev.file_path = (m_EventPaths.Size() > 0) ? m_EventPaths[0].Data() : nullptr;

        for (usize ci = 0; ci < m_Callbacks.Size(); ++ci) {
            const CallbackEntry& c = m_Callbacks[ci];
            if (c.cb != nullptr) {
                c.cb(c.user, ev);
            }
        }

        // dispatch 済み先頭を物理削除 (lockstep shift)。
        RemoveFrontEventPair();
    }
}

/** 監視登録された path の数を返す。 */
u32 HotReloadWatcher::WatchedCount() const noexcept {
    return static_cast<u32>(m_WatchedPaths.Size());
}

/** 未消費の pending event 数を返す。 */
u32 HotReloadWatcher::PendingEventCount() const noexcept {
    return static_cast<u32>(m_PendingEvents.Size());
}

/** pending event の先頭 1 件を FIFO で取り出して除去する (空なら false)。 */
bool HotReloadWatcher::ConsumeNextEvent(HotReloadEvent& out) noexcept {
    if (m_PendingEvents.Size() == 0) {
        return false;  // 空なら out は触らず false
    }

    // FIFO 順で先頭を取り出す。file_path は cache せず、対応する所有文字列
    // (m_EventPaths[0]) の現行アドレスから解決する。返した out.file_path は
    // 「次に Consume / ClearEvents / Shutdown するまで」valid。
    out = m_PendingEvents[0];
    out.file_path = (m_EventPaths.Size() > 0) ? m_EventPaths[0].Data() : nullptr;

    // 先頭を物理削除して shift-left (m_PendingEvents / m_EventPaths を lockstep)。
    // pending size は <= 数十想定なので shift コストは実用上無視できる。
    // (swap-remove は順序を壊すため不採用 — hot reload は「最初に届いた変更を
    //  最初に処理する」のが直感的)
    RemoveFrontEventPair();
    return true;
}

/** pending event 先頭 1 件を m_PendingEvents / m_EventPaths から lockstep で shift 除去する。 */
void HotReloadWatcher::RemoveFrontEventPair() noexcept {
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

/** pending event と所有 path 文字列を lockstep で全クリアする。 */
void HotReloadWatcher::ClearEvents() noexcept {
    // pending events と所有 path 文字列を lockstep で全クリア。
    m_PendingEvents.Clear();
    m_EventPaths.Clear();
}

#else // ACS_GAME_SHIPPING

// Ship build (ACS_GAME_SHIPPING) では全 method を no-op にする。シンボル定義は残し、
// 呼び出し側コードが #ifdef だらけにならないようにする。戻り値は安全な既定値 (0 / false)。

/** ship build のデストラクタ (no-op)。 */
HotReloadWatcher::~HotReloadWatcher() noexcept {}

/** ship build では監視を行わない (no-op)。 */
void HotReloadWatcher::Init() noexcept {}

/** ship build では解放するものが無い (no-op)。 */
void HotReloadWatcher::Shutdown() noexcept {}

/** ship build ではディレクトリ監視を行わない (no-op)。 */
void HotReloadWatcher::WatchDirectory(const char* /*dir_path*/, bool /*recursive*/) noexcept {}

/** ship build ではファイル監視を行わない (no-op)。 */
void HotReloadWatcher::WatchFile(const char* /*file_path*/) noexcept {}

/** ship build では監視解除するものが無い (no-op)。 */
void HotReloadWatcher::Unwatch(const char* /*path*/) noexcept {}

/** ship build では callback を登録しない (no-op)。 */
void HotReloadWatcher::RegisterCallback(HotReloadCallback /*cb*/, void* /*user*/) noexcept {}

/** ship build では駆動しない (no-op)。 */
void HotReloadWatcher::Tick(f32 /*dt*/) noexcept {}

/**
 * ship build の監視 path 数 (常に 0)。
 *
 * @return 0。
 */
u32  HotReloadWatcher::WatchedCount() const noexcept { return 0; }

/**
 * ship build の pending event 数 (常に 0)。
 *
 * @return 0。
 */
u32  HotReloadWatcher::PendingEventCount() const noexcept { return 0; }

/**
 * ship build の event 取り出し (常に失敗)。
 *
 * @param out 触らない。
 * @return false。
 */
bool HotReloadWatcher::ConsumeNextEvent(HotReloadEvent& /*out*/) noexcept { return false; }

/** ship build では event を持たない (no-op)。 */
void HotReloadWatcher::ClearEvents() noexcept {}

#endif // ACS_GAME_SHIPPING

} // namespace acs::game
