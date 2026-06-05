// SPDX-License-Identifier: Apache-2.0
// 非同期スレッドセーフ FLogger 実装
#include "foundation/Log.h"
#include "foundation/Platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace acs {

namespace {

/** 1 メッセージの最大バイト長 (これを超えると切り詰める)。 */
constexpr u32 kMessageMax = 480;

/**
 * 1 ログレコードを保持する固定サイズスロット。
 *
 * @details 64B 整列でフォルスシェアリングを避ける。sequence で Vyukov 風 CAS 調停を行う。
 */
struct alignas(64) Cell {
    /** CAS 調停用シーケンス番号 (空き / コミット済みの状態を表す)。 */
    volatile LONG64 sequence;

    /** レコードの重大度。 */
    ELogSeverity     severity;

    /** severity と loc の整列を保つためのパディング。 */
    u8              m_Pad0[7];

    /** 呼び出し位置 (ファイル・行・関数)。 */
    FSourceLoc       loc;

    /** 発生スレッドの ID。 */
    DWORD           thread_id;

    /** 発生時刻 (QPC ティック)。 */
    LARGE_INTEGER   timestamp;

    /** メッセージ本文の有効バイト数。 */
    u16             message_len;

    /** メッセージ本文 (最大 kMessageMax バイト)。 */
    char            message[kMessageMax];
};

static_assert(sizeof(Cell) % 64 == 0 || sizeof(Cell) >= 64, "Cell should be at least one cache line");

/**
 * ロガーのグローバル状態 (リング・カーソル・出力先・時刻較正をまとめる)。
 */
struct LoggerState {
    /** リングバッファ先頭 (VirtualAlloc で確保)。 */
    Cell*        ring          = nullptr;

    /** リングの要素数 (2 のべき乗)。 */
    u32          capacity      = 0;

    /** capacity - 1。index & mask で循環インデックスを得る。 */
    u32          mask          = 0;

    /** プロデューサ側カーソル (次に積む位置、別キャッシュラインに配置)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 enqueue_pos = 0;

    /** コンシューマ側カーソル (次に取り出す位置、別キャッシュラインに配置)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dequeue_pos = 0;

    /** 未警告ぶんの drop バッチ件数 (writer が周回毎に 0 化する)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dropped       = 0;

    /** 累積 drop 件数 (DroppedCount 用。決して 0 化しない)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dropped_total = 0;

    /** ホットパスの severity ゲート (別キャッシュラインに配置した最小レベル)。 */
    ACS_CACHELINE_ALIGN volatile LONG min_severity = static_cast<LONG>(ELogSeverity::Info);

    /** ライタースレッドのハンドル。 */
    HANDLE              writer_thread     = nullptr;

    /** ライタースレッドの稼働フラグ (0 で停止要求)。 */
    volatile LONG       running           = 0;

    /** ライター起床用 CV と対になるロック。 */
    SRWLOCK             wake_lock         = SRWLOCK_INIT;

    /** ライタースレッドを起こす条件変数。 */
    CONDITION_VARIABLE  wake_cv           = CONDITION_VARIABLE_INIT;

    /** コンソール出力先ハンドル。 */
    HANDLE  out_console = INVALID_HANDLE_VALUE;

    /** ファイル出力先ハンドル。 */
    HANDLE  out_file    = INVALID_HANDLE_VALUE;

    /** コンソール出力が有効か。 */
    bool    use_console = false;

    /** OutputDebugStringA 出力が有効か。 */
    bool    use_dbgout  = false;

    /** QPC の周波数 (ティック → 秒の換算用)。 */
    LARGE_INTEGER qpc_freq {};

    /** 較正基準時刻の QPC 値。 */
    LARGE_INTEGER qpc_origin {};

    /** 較正基準時刻の壁時計 (FILETIME)。 */
    FILETIME      ft_origin {};
};

/** ロガーのグローバル状態インスタンス。 */
LoggerState g_state;

/** ready フラグ: producer はこれが 1 のときだけ ring に触れる (全設定完了後に立てる)。 */
volatile LONG g_inited    = 0;

/** once ガード: Init / Shutdown の二重実行を防ぐ。 */
volatile LONG g_init_lock = 0;

/**
 * 2 のべき乗かを判定する。
 *
 * @param v 判定する値。
 * @return v が 0 でなく 2 のべき乗なら true。
 */
ACS_FORCEINLINE bool IsPowerOfTwo(u32 v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

/**
 * HANDLE へ全バイトを書き出す (部分書き込みに対応してループ)。
 *
 * @param h 書き込み先ハンドル (無効ハンドルなら何もしない)。
 * @param p 書き込むバッファ。
 * @param n 書き込むバイト数。
 */
void WriteAll(HANDLE h, const char* p, usize n) noexcept {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    while (n > 0) {
        DWORD chunk = static_cast<DWORD>(n > 0xFFFF0000u ? 0xFFFF0000u : n);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) return;
        p += wrote;
        n -= wrote;
    }
}

/**
 * QPC ティックを「YYYY-MM-DD HH:MM:SS.mmm」のローカル時刻文字列に変換する。
 *
 * @details
 * 較正基準 (qpc_origin / ft_origin) からの差分を 100ns 単位で加算して壁時計に直す。
 * delta*1e7 を直接やると稼働 ~25h で i64 overflow するため、秒部と剰余部に分けて計算する。
 * @param qpc 変換する QPC ティック値。
 * @param out 整形先バッファ。
 * @param cap out のバイト容量。
 */
void FormatTimestamp(const LARGE_INTEGER& qpc, char* out, usize cap) noexcept {
    LONGLONG delta = qpc.QuadPart - g_state.qpc_origin.QuadPart;
    const LONGLONG freq = g_state.qpc_freq.QuadPart > 0 ? g_state.qpc_freq.QuadPart : 1;
    // delta*10000000 を直接やると freq~10MHz では稼働 ~25h で i64 overflow するため、
    // 秒部と剰余部に分けて 100ns 単位へ変換する (どちらも overflow しない)。
    const LONGLONG sec = delta / freq;
    const LONGLONG rem = delta % freq;
    const LONGLONG ns100 = sec * 10000000LL + (rem * 10000000LL) / freq;

    ULARGE_INTEGER ft;
    ft.LowPart  = g_state.ft_origin.dwLowDateTime;
    ft.HighPart = g_state.ft_origin.dwHighDateTime;
    ft.QuadPart += static_cast<ULONGLONG>(ns100);

    FILETIME    ftloc;
    SYSTEMTIME  st;
    FILETIME    ftnow {};
    ftnow.dwLowDateTime  = ft.LowPart;
    ftnow.dwHighDateTime = ft.HighPart;
    ::FileTimeToLocalFileTime(&ftnow, &ftloc);
    ::FileTimeToSystemTime(&ftloc, &st);

    ::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/**
 * 1 セルを 1 行に整形し、有効な全出力先 (コンソール / ファイル / デバッガ) に書き出す。
 *
 * @param c 出力するログレコード。
 */
void EmitOne(const Cell& c) noexcept {
    char ts[32];
    FormatTimestamp(c.timestamp, ts, sizeof(ts));

    // [時刻] [レベル] [tid] file:line (func) | message
    char line[1024];
    const int n = ::snprintf(line, sizeof(line),
        "[%s] [%-5s] [tid=%lu] %s:%u (%s) | %.*s\n",
        ts, ToString(c.severity), static_cast<unsigned long>(c.thread_id),
        c.loc.File(), c.loc.Line(), c.loc.Function(),
        static_cast<int>(c.message_len), c.message);
    if (n < 0) return;
    const usize len = static_cast<usize>(n) < sizeof(line) ? static_cast<usize>(n) : sizeof(line) - 1;

    if (g_state.use_console) WriteAll(g_state.out_console, line, len);
    if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, line, len);
    if (g_state.use_dbgout) ::OutputDebugStringA(line);
}

/**
 * ライタースレッド本体。リングを空になるまでドレインし、drop 警告を出し、スリープする。
 *
 * @details running が 0 かつリングが空になったら終了する。起床は CV または 100ms タイムアウト。
 * @return スレッド終了コード (常に 0)。
 */
DWORD WINAPI WriterThreadProc(LPVOID) noexcept {
    while (true) {
        // === ドレインループ: リングを空になるまで読み出す ===
        for (;;) {
            LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            Cell& cell = g_state.ring[pos & g_state.mask];
            LONG64 seq = ::_InterlockedExchangeAdd64(&cell.sequence, 0);
            LONG64 dif = seq - (pos + 1);
            if (dif == 0) {
                // pos のセルは commit 済み — 取り出し成功
                if (::_InterlockedCompareExchange64(&g_state.dequeue_pos, pos + 1, pos) == pos) {
                    EmitOne(cell);
                    // 次の周回のためにシーケンスを進める
                    ::_InterlockedExchange64(&cell.sequence, pos + g_state.capacity);
                }
            } else if (dif < 0) {
                break;  // 空 — ドレイン終了
            }
            // dif > 0 は競合（シングルコンシューマだから通常起きない）
        }

        // === drop 件数があれば警告を出す ===
        const LONG64 dropped = ::_InterlockedExchange64(&g_state.dropped, 0);
        if (dropped > 0) {
            char warn[160];
            const int n = ::snprintf(warn, sizeof(warn),
                "[acs::FLogger] WARNING: dropped %lld log records due to ring overflow\n",
                static_cast<long long>(dropped));
            if (n > 0) {
                if (g_state.use_console) WriteAll(g_state.out_console, warn, static_cast<usize>(n));
                if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, warn, static_cast<usize>(n));
                if (g_state.use_dbgout) ::OutputDebugStringA(warn);
            }
        }

        // === シャットダウン判定 ===
        if (!::_InterlockedExchangeAdd(&g_state.running, 0)) {
            // running=0 で、残レコードもなければ終了
            const LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
            const LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            if (tail >= head) break;
            continue;  // まだ残っているのでドレイン継続
        }

        // === スリープ（CV 起床 or 100ms タイムアウト） ===
        AcquireSRWLockExclusive(&g_state.wake_lock);
        ::SleepConditionVariableSRW(&g_state.wake_cv, &g_state.wake_lock, 100, 0);
        ReleaseSRWLockExclusive(&g_state.wake_lock);
    }

    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(g_state.out_file);
    }
    return 0;
}

} // namespace

/** ロガーを初期化する。詳細は宣言を参照。 */
void FLogger::Init(const FLogConfig& cfg) noexcept {
    // once ガードは g_init_lock で取る。g_inited (ready) は ring/threads を全部
    // 用意し終えた末尾でのみ立てる。これをしないと、CAS 直後〜ring 確保前の窓で
    // 別スレッドの Write() が g_inited==1 を見て nullptr ring を deref する。
    if (::_InterlockedCompareExchange(&g_init_lock, 1, 0) != 0) return;

    // capacity を 2 のべき乗（最低 16）に補正
    u32 cap = cfg.ring_capacity;
    if (!IsPowerOfTwo(cap)) {
        u32 v = 1; while (v < cap) v <<= 1; cap = v;
    }
    if (cap < 16) cap = 16;

    // ページ単位でリング確保
    void* const mem = ::VirtualAlloc(nullptr, sizeof(Cell) * cap,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        ::OutputDebugStringA("[acs::FLogger] FATAL: ring allocation failed\n");
        ::_InterlockedExchange(&g_init_lock, 0);   // 失敗 → 再 Init を許可 (g_inited は未設定のまま)
        return;
    }
    g_state.ring     = static_cast<Cell*>(mem);
    g_state.capacity = cap;
    g_state.mask     = cap - 1;
    // 各セルのシーケンス番号を i 番目=i から開始
    for (u32 i = 0; i < cap; ++i) {
        g_state.ring[i].sequence = i;
    }
    g_state.enqueue_pos = 0;
    g_state.dequeue_pos = 0;
    g_state.dropped     = 0;

    // 出力先ハンドル取得
    g_state.use_console = cfg.console;
    g_state.use_dbgout  = cfg.debug_output;
    if (cfg.console) g_state.out_console = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (cfg.file_path) {
        g_state.out_file = ::CreateFileW(cfg.file_path, GENERIC_WRITE, FILE_SHARE_READ,
                                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // QPC ↔ wall clock キャリブレーション
    ::QueryPerformanceFrequency(&g_state.qpc_freq);
    ::QueryPerformanceCounter(&g_state.qpc_origin);
    ::GetSystemTimeAsFileTime(&g_state.ft_origin);

    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(cfg.min_severity));
    ::_InterlockedExchange(&g_state.running, 1);

    // ライタースレッド起動
    g_state.writer_thread = ::CreateThread(nullptr, 0, &WriterThreadProc, nullptr, 0, nullptr);
    ::SetThreadDescription(g_state.writer_thread, L"acs::FLogger writer");

    // 全状態 (ring/mask/threads/calibration) を公開し終えた最後に ready を立てる。
    ::_InterlockedExchange(&g_inited, 1);
}

/** ライタースレッドを停止しリソースを解放する。詳細は宣言を参照。 */
void FLogger::Shutdown() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;

    // ring 解放より先に ready を下げ、新規 producer を ring から締め出す
    // (free-before-clear だと g_inited==1 のまま解放済み ring を UAF し得る)。
    ::_InterlockedExchange(&g_inited, 0);
    ::_InterlockedExchange(&g_state.running, 0);
    ::WakeAllConditionVariable(&g_state.wake_cv);
    if (g_state.writer_thread) {
        ::WaitForSingleObject(g_state.writer_thread, INFINITE);
        ::CloseHandle(g_state.writer_thread);
        g_state.writer_thread = nullptr;
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_state.out_file);
        g_state.out_file = INVALID_HANDLE_VALUE;
    }
    if (g_state.ring) {
        ::VirtualFree(g_state.ring, 0, MEM_RELEASE);
        g_state.ring = nullptr;
    }
    ::_InterlockedExchange(&g_init_lock, 0);   // 再 Init を許可
}

/** 残レコードを書き出すまで待つ。詳細は宣言を参照。 */
void FLogger::Flush() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    // 最大 1000 回リトライ（1ms x 1000 = 1秒）
    for (int i = 0; i < 1000; ++i) {
        const LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        const LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
        if (tail >= head) break;
        ::WakeAllConditionVariable(&g_state.wake_cv);
        ::Sleep(1);
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_state.out_file);
}

void FLogger::SetMinSeverity(ELogSeverity s) noexcept {
    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(s));
}

bool FLogger::Enabled(ELogSeverity s) noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return false;
    return static_cast<LONG>(s) >= ::_InterlockedExchangeAdd(&g_state.min_severity, 0);
}

u64 FLogger::DroppedCount() noexcept {
    // writer は g_state.dropped を周回毎に 0 化するため、累積は dropped_total を返す。
    return static_cast<u64>(::_InterlockedExchangeAdd64(&g_state.dropped_total, 0));
}

/** プロデューサ実体: Vyukov 風 CAS でセルを予約し、書き込んで release 公開する。詳細は宣言を参照。 */
void FLogger::Write(ELogSeverity sev, FSourceLoc loc, const char* fmt, ...) noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    if (static_cast<LONG>(sev) < ::_InterlockedExchangeAdd(&g_state.min_severity, 0)) return;

    // === スロット予約（CAS ループ） ===
    LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
    Cell* cell = nullptr;
    while (true) {
        cell = &g_state.ring[pos & g_state.mask];
        LONG64 seq = ::_InterlockedExchangeAdd64(&cell->sequence, 0);
        LONG64 dif = seq - pos;
        if (dif == 0) {
            // セルが空なら pos を進めて確保
            if (::_InterlockedCompareExchange64(&g_state.enqueue_pos, pos + 1, pos) == pos) break;
        } else if (dif < 0) {
            // リング満杯なら drop (未警告バッチと累積の両方を加算)
            ::_InterlockedIncrement64(&g_state.dropped);
            ::_InterlockedIncrement64(&g_state.dropped_total);
            return;
        } else {
            // 他プロデューサが先に取った — pos を再読み込み
            pos = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        }
    }

    // === セル内容を書き込み ===
    cell->severity  = sev;
    cell->loc       = loc;
    cell->thread_id = ::GetCurrentThreadId();
    ::QueryPerformanceCounter(&cell->timestamp);

    va_list ap;
    va_start(ap, fmt);
    int n = ::vsnprintf(cell->message, kMessageMax, fmt ? fmt : "(null)", ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (static_cast<u32>(n) >= kMessageMax) n = static_cast<int>(kMessageMax - 1);
    cell->message_len = static_cast<u16>(n);

    // === 公開（release ストア） ===
    // sequence = pos + 1 でライタに「コミット済み」を通知
    ::_InterlockedExchange64(&cell->sequence, pos + 1);

    // ライタが寝ていたら起こす
    ::WakeConditionVariable(&g_state.wake_cv);
}

} // namespace acs
