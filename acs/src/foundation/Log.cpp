// =============================================================================
// ACS Foundation — Logger 実装
// -----------------------------------------------------------------------------
// アーキテクチャ:
//   - Cell 配列: Vyukov 境界付き MPMC リング（固定サイズ、2 のべき乗）
//   - プロデューサ: シーケンス番号方式の CAS で空きセルを予約 → 内容書き込み
//                   → release ストアで「コミット済み」を公開
//   - コンシューマ: 専用ライタースレッドが順番にドレインして実出力
//   - 出力先: stdout / OutputDebugString / ファイル
//
// 用語:
//   enqueue_pos / dequeue_pos: グローバル ヘッド / テイル カウンタ
//   sequence: 各セルが「自分が今どの周回にあるか」を保持する番号
// =============================================================================
#include "foundation/Log.h"
#include "foundation/Platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace acs {

namespace {

// ---- セル ----------------------------------------------------------------
// 各ログレコードを格納する固定サイズスロット。
// プロデューサ／コンシューマ間の調停用にシーケンス番号を持つ。
// 64 バイト整列でフォルスシェアリングを抑える。
constexpr u32 kMessageMax = 480;  // 1 メッセージ最大長

struct alignas(64) Cell {
    volatile LONG64 sequence;       // CAS 調停用シーケンス番号
    LogSeverity     severity;       // ログレベル
    u8              _pad0[7];       // 整列パディング
    SourceLoc       loc;            // 発生位置
    DWORD           thread_id;      // 出力元スレッド ID
    LARGE_INTEGER   timestamp;      // QPC ティック（ライタが時刻に変換）
    u16             message_len;    // 実メッセージ長
    char            message[kMessageMax]; // メッセージ本文
};

static_assert(sizeof(Cell) % 64 == 0 || sizeof(Cell) >= 64, "Cell should be at least one cache line");

// ---- グローバル状態 ------------------------------------------------------
struct LoggerState {
    Cell*        ring          = nullptr;  // リングバッファ実体
    u32          capacity      = 0;        // セル数
    u32          mask          = 0;        // (capacity - 1) — index & mask で循環

    // プロデューサとコンシューマのカーソルは別キャッシュラインに置き、
    // フォルスシェアリングを完全に回避する（ACS_CACHELINE_ALIGN）。
    ACS_CACHELINE_ALIGN volatile LONG64 enqueue_pos = 0;  // 次に書き込むスロット
    ACS_CACHELINE_ALIGN volatile LONG64 dequeue_pos = 0;  // 次に読み出すスロット
    ACS_CACHELINE_ALIGN volatile LONG64 dropped     = 0;  // 満杯で破棄した数

    // ホットパスの severity ゲートも別ラインに（Read のみだが念のため）。
    ACS_CACHELINE_ALIGN volatile LONG min_severity = static_cast<LONG>(LogSeverity::Info);

    // ライタースレッド関連
    HANDLE              writer_thread     = nullptr;
    volatile LONG       running           = 0;                       // 1=動作中, 0=停止
    SRWLOCK             wake_lock         = SRWLOCK_INIT;            // CV 用
    CONDITION_VARIABLE  wake_cv           = CONDITION_VARIABLE_INIT; // ライタ起床通知

    // 出力先ハンドル
    HANDLE  out_console = INVALID_HANDLE_VALUE;
    HANDLE  out_file    = INVALID_HANDLE_VALUE;
    bool    use_console = false;
    bool    use_dbgout  = false;

    // QPC → wall clock 変換用キャリブレーション
    LARGE_INTEGER qpc_freq {};
    LARGE_INTEGER qpc_origin {};
    FILETIME      ft_origin {};
};

LoggerState g_state;
volatile LONG g_inited = 0;  // Init 多重呼び出し防止

// 2 のべき乗判定
ACS_FORCEINLINE bool IsPowerOfTwo(u32 v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

// HANDLE への完全書き出し（部分書き込み対応）
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

// QPC ティックを「YYYY-MM-DD HH:MM:SS.mmm」形式の文字列に変換する。
// Init 時に取った QPC 原点と FILETIME 原点の差分を加算するだけ。
void FormatTimestamp(const LARGE_INTEGER& qpc, char* out, usize cap) noexcept {
    LONGLONG delta = qpc.QuadPart - g_state.qpc_origin.QuadPart;
    LONGLONG ns100 = (delta * 10000000LL) / g_state.qpc_freq.QuadPart;  // 100ns 単位

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

// 1 セルを実際にフォーマットして全出力先に書き出す。
void EmitOne(const Cell& c) noexcept {
    char ts[32];
    FormatTimestamp(c.timestamp, ts, sizeof(ts));

    // 1 行のフォーマット: [時刻] [レベル] [tid] file:line (func) | message
    char line[1024];
    int n = ::snprintf(line, sizeof(line),
        "[%s] [%-5s] [tid=%lu] %s:%u (%s) | %.*s\n",
        ts, ToString(c.severity), static_cast<unsigned long>(c.thread_id),
        c.loc.File(), c.loc.Line(), c.loc.Function(),
        static_cast<int>(c.message_len), c.message);
    if (n < 0) return;
    usize len = static_cast<usize>(n) < sizeof(line) ? static_cast<usize>(n) : sizeof(line) - 1;

    if (g_state.use_console) WriteAll(g_state.out_console, line, len);
    if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, line, len);
    if (g_state.use_dbgout) ::OutputDebugStringA(line);
}

// ライタースレッド本体。
// 1) リングを空になるまでドレイン
// 2) drop された件数があれば警告を出力
// 3) running=0 なら最後のドレイン後に終了
// 4) それ以外は CV で 100ms までスリープし、目覚めたら 1) へ
DWORD WINAPI WriterThreadProc(LPVOID) noexcept {
    while (true) {
        // === ドレインループ ===
        for (;;) {
            LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            Cell& cell = g_state.ring[pos & g_state.mask];
            LONG64 seq = ::_InterlockedExchangeAdd64(&cell.sequence, 0);
            LONG64 dif = seq - (pos + 1);
            if (dif == 0) {
                // pos のセルが「commit 済み」状態 — 取り出し成功
                if (::_InterlockedCompareExchange64(&g_state.dequeue_pos, pos + 1, pos) == pos) {
                    EmitOne(cell);
                    // 次の周回のためにシーケンスを更新（capacity 加算）
                    ::_InterlockedExchange64(&cell.sequence, pos + g_state.capacity);
                }
            } else if (dif < 0) {
                break;  // 空 — ドレイン終了
            }
            // dif > 0: 競合 — 再試行（実際にはシングルコンシューマなので発生しない）
        }

        // === drop 件数の通知 ===
        LONG64 dropped = ::_InterlockedExchange64(&g_state.dropped, 0);
        if (dropped > 0) {
            char warn[160];
            int n = ::snprintf(warn, sizeof(warn),
                "[acs::Logger] WARNING: dropped %lld log records due to ring overflow\n",
                static_cast<long long>(dropped));
            if (n > 0) {
                if (g_state.use_console) WriteAll(g_state.out_console, warn, static_cast<usize>(n));
                if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, warn, static_cast<usize>(n));
                if (g_state.use_dbgout) ::OutputDebugStringA(warn);
            }
        }

        // === シャットダウン判定 ===
        if (!::_InterlockedExchangeAdd(&g_state.running, 0)) {
            // 残レコードがなければ終了
            LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
            LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            if (tail >= head) break;
            continue;  // まだ残っているのでドレインを継続
        }

        // === スリープ ===
        // CV で起こされるか 100ms タイムアウトで再開
        AcquireSRWLockExclusive(&g_state.wake_lock);
        ::SleepConditionVariableSRW(&g_state.wake_cv, &g_state.wake_lock, 100, 0);
        ReleaseSRWLockExclusive(&g_state.wake_lock);
    }

    // ファイルがあれば最終フラッシュ
    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(g_state.out_file);
    }
    return 0;
}

} // namespace

// =============================================================================
// 公開 API
// =============================================================================

// 初期化。多重呼び出しは無視される。失敗しても処理は続行（無効ロガーになる）。
void Logger::Init(const LogConfig& cfg) noexcept {
    if (::_InterlockedCompareExchange(&g_inited, 1, 0) != 0) return;

    // capacity を 2 のべき乗に補正（最低 16）
    u32 cap = cfg.ring_capacity;
    if (!IsPowerOfTwo(cap)) {
        u32 v = 1; while (v < cap) v <<= 1; cap = v;
    }
    if (cap < 16) cap = 16;

    // ページ単位でリング確保（VirtualAlloc は 4KB 整列を保証）
    void* mem = ::VirtualAlloc(nullptr, sizeof(Cell) * cap,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        ::OutputDebugStringA("[acs::Logger] FATAL: ring allocation failed\n");
        ::_InterlockedExchange(&g_inited, 0);
        return;
    }
    g_state.ring     = static_cast<Cell*>(mem);
    g_state.capacity = cap;
    g_state.mask     = cap - 1;
    // 各セルのシーケンス番号を初期化（i 番目のセルは sequence=i から開始）
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

    // ライタースレッドを起動
    g_state.writer_thread = ::CreateThread(nullptr, 0, &WriterThreadProc, nullptr, 0, nullptr);
    ::SetThreadDescription(g_state.writer_thread, L"acs::Logger writer");
}

// シャットダウン。ライタを停止し、リソースを解放する。
void Logger::Shutdown() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;

    ::_InterlockedExchange(&g_state.running, 0);
    ::WakeAllConditionVariable(&g_state.wake_cv);  // ライタを起こす
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
    ::_InterlockedExchange(&g_inited, 0);
}

// 同期点。残ログを書き出すまで待つ（テスト終了時等で呼ぶ）。
void Logger::Flush() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    // 最大 1000 回リトライ（1ms x 1000 = 1秒）
    for (int i = 0; i < 1000; ++i) {
        LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
        if (tail >= head) break;
        ::WakeAllConditionVariable(&g_state.wake_cv);
        ::Sleep(1);
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_state.out_file);
}

void Logger::SetMinSeverity(LogSeverity s) noexcept {
    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(s));
}

bool Logger::Enabled(LogSeverity s) noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return false;
    return static_cast<LONG>(s) >= ::_InterlockedExchangeAdd(&g_state.min_severity, 0);
}

u64 Logger::DroppedCount() noexcept {
    return static_cast<u64>(::_InterlockedExchangeAdd64(&g_state.dropped, 0));
}

// プロデューサ実体。Vyukov 風 CAS でセルを予約 → 書き込み → release 公開。
void Logger::Write(LogSeverity sev, SourceLoc loc, const char* fmt, ...) noexcept {
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
            // セルが空 — pos を進めて確保
            if (::_InterlockedCompareExchange64(&g_state.enqueue_pos, pos + 1, pos) == pos) break;
        } else if (dif < 0) {
            // リング満杯 — drop
            ::_InterlockedIncrement64(&g_state.dropped);
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
    // sequence = pos + 1 は「このセルはコミット済み」をライタに知らせる。
    ::_InterlockedExchange64(&cell->sequence, pos + 1);

    // ライタが寝ていたら起こす（NotifyOne）。
    ::WakeConditionVariable(&g_state.wake_cv);
}

} // namespace acs
