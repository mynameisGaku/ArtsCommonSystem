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

/** リング容量の下限。 */
constexpr u32 kMinimumRingCapacity = 16;

/** 異常な設定値による桁あふれと過大な常駐領域を防ぐ上限。 */
constexpr u32 kMaximumRingCapacity = 1u << 16;

/**
 * 1 ログレコードを保持する固定サイズスロット。
 *
 * @details 64B 整列でフォルスシェアリングを避ける。sequence で Vyukov 風 CAS 調停を行う。
 */
struct alignas(64) FCell {
    /** CAS 調停用シーケンス番号 (空き / コミット済みの状態を表す)。 */
    volatile LONG64 sequence;

    /** レコードの重大度。 */
    ELogSeverity severity;

    /** severity と loc の整列を保つためのパディング。 */
    u8 m_Pad0[7];

    /** 呼び出し位置 (ファイル・行・関数)。 */
    FSourceLoc loc;

    /** 発生スレッドの ID。 */
    DWORD thread_id;

    /** 発生時刻 (QPC ティック)。 */
    LARGE_INTEGER timestamp;

    /** メッセージ本文の有効バイト数。 */
    u16 message_len;

    /** メッセージ本文 (最大 kMessageMax バイト)。 */
    char message[kMessageMax];
};

static_assert(sizeof(FCell) % 64 == 0 || sizeof(FCell) >= 64, "Cell should be at least one cache line");

/**
 * ロガーのグローバル状態 (リング・カーソル・出力先・時刻較正をまとめる)。
 */
struct FLoggerState {
    /** リングバッファ先頭 (VirtualAlloc で確保)。 */
    FCell* ring = nullptr;

    /** リングの要素数 (2 のべき乗)。 */
    u32 capacity = 0;

    /** capacity - 1。index & mask で循環インデックスを得る。 */
    u32 mask = 0;

    /** プロデューサ側カーソル (次に積む位置、別キャッシュラインに配置)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 enqueue_pos = 0;

    /** コンシューマ側カーソル (次に取り出す位置、別キャッシュラインに配置)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dequeue_pos = 0;

    /** 出力先と追加シンクへの通知を完了した次の位置。 */
    ACS_CACHELINE_ALIGN volatile LONG64 completed_emit_pos = 0;

    /** 未警告ぶんの drop バッチ件数 (writer が周回毎に 0 化する)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dropped = 0;

    /** 累積 drop 件数 (DroppedCount 用。決して 0 化しない)。 */
    ACS_CACHELINE_ALIGN volatile LONG64 dropped_total = 0;

    /** 通常レコード投入によって送った起床通知の累積数。 */
    ACS_CACHELINE_ALIGN volatile LONG64 wake_signal_count = 0;

    /** ホットパスの severity ゲート (別キャッシュラインに配置した最小レベル)。 */
    ACS_CACHELINE_ALIGN volatile LONG min_severity = static_cast<LONG>(ELogSeverity::Info);

    /** ライタースレッドのハンドル。 */
    HANDLE writer_thread = nullptr;

    /** ライタースレッドの稼働フラグ (0 で停止要求)。 */
    volatile LONG running = 0;

    /** ライター起床用 CV と対になるロック。 */
    SRWLOCK wake_lock = SRWLOCK_INIT;

    /** ライタースレッドを起こす条件変数。 */
    CONDITION_VARIABLE wake_cv = CONDITION_VARIABLE_INIT;

    /** コンソール出力先ハンドル。 */
    HANDLE out_console = INVALID_HANDLE_VALUE;

    /** ファイル出力先ハンドル。 */
    HANDLE out_file = INVALID_HANDLE_VALUE;

    /** コンソール出力が有効か。 */
    bool use_console = false;

    /** OutputDebugStringA 出力が有効か。 */
    bool use_dbgout = false;

    /** 追加のログシンク (コールバック)。EmitOne が各レコードで呼ぶ (null=無効)。 */
    void* volatile sink = nullptr;

    /** QPC の周波数 (ティック → 秒の換算用)。 */
    LARGE_INTEGER qpc_freq{};

    /** 較正基準時刻の QPC 値。 */
    LARGE_INTEGER qpc_origin{};

    /** 較正基準時刻の壁時計 (FILETIME)。 */
    FILETIME ft_origin{};
};

/** ロガーのグローバル状態インスタンス。 */
FLoggerState g_state;

/** ready フラグ: producer はこれが 1 のときだけ ring に触れる (全設定完了後に立てる)。 */
volatile LONG g_inited = 0;

/** Init / Shutdown / SetSink と所有資源の切替を直列化する。 */
SRWLOCK g_lifecycle_lock = SRWLOCK_INIT;

/** ready 確認を通過し、リングへ触れる可能性がある producer 数。 */
volatile LONG g_active_producers = 0;

/** 再初期化をまたぐ Write が別 lifecycle の ring へ入らないための世代。 */
volatile LONG g_lifecycle_generation = 0;

/** sink pointer の交換と callback 実行を世代境界で分離する。 */
SRWLOCK g_sink_callback_lock = SRWLOCK_INIT;

/** sink 自身から SetSink を呼ぶ場合の自己待機を避ける。 */
thread_local bool t_inside_sink_callback = false;

/** Write の全 return 経路で producer 数を確実に戻すスコープガード。 */
struct FProducerGuard {
    ~FProducerGuard() noexcept
    {
        ::_InterlockedDecrement(&g_active_producers);
    }
};

/** プロデューサが確保したリング位置とセルをまとめる。 */
struct FRecordReservation {
    /** 書き込み権を得たリングセル。 */
    FCell* cell = nullptr;

    /** 単調増加するリング位置。 */
    LONG64 position = 0;
};

/**
 * 空きセルを確保し、共通のレコード情報を設定する。
 *
 * 呼び出し側が g_active_producers を登録済みで、同じ世代が有効な間だけ呼ぶ。
 *
 * @param severity 出力する重大度。
 * @param location 呼び出し元のソース位置。
 * @param reservation 確保したセルと位置の出力先。
 * @return セルを確保できた場合は true。満杯なら破棄件数を加算して false。
 */
bool ReserveRecord(ELogSeverity severity, FSourceLoc location, FRecordReservation& reservation) noexcept
{
    /** 次に確保を試みる単調増加位置。 */
    LONG64 position = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
    /** 現在確認しているリングセル。 */
    FCell* cell = nullptr;
    while (true) {
        cell = &g_state.ring[position & g_state.mask];
        /** セルが再利用可能になる単調増加位置。 */
        const LONG64 sequence = ::_InterlockedExchangeAdd64(&cell->sequence, 0);
        /** セルの公開状態と要求位置の差。 */
        const LONG64 difference = sequence - position;
        if (difference == 0) {
            if (::_InterlockedCompareExchange64(&g_state.enqueue_pos, position + 1, position) == position) {
                break;
            }
        } else if (difference < 0) {
            ::_InterlockedIncrement64(&g_state.dropped);
            ::_InterlockedIncrement64(&g_state.dropped_total);
            return false;
        } else {
            position = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        }
    }

    cell->severity = severity;
    cell->loc = location;
    cell->thread_id = ::GetCurrentThreadId();
    ::QueryPerformanceCounter(&cell->timestamp);
    reservation.cell = cell;
    reservation.position = position;
    return true;
}

/**
 * 完成したセルを公開し、空から非空になった場合だけライターを起こす。
 *
 * wake_lock を共有取得することで、ライターの空判定と CV 待機の間に通知が失われない。
 *
 * @param reservation 公開するセルと単調増加位置。
 */
void PublishRecord(const FRecordReservation& reservation) noexcept
{
    ::_InterlockedExchange64(&reservation.cell->sequence, reservation.position + 1);

    /** 公開時点でコンシューマが次に読む位置。 */
    const LONG64 dequeue_position = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
    if (!detail::ShouldSignalLogConsumer(static_cast<u64>(reservation.position), static_cast<u64>(dequeue_position))) {
        return;
    }

    ::AcquireSRWLockShared(&g_state.wake_lock);
    /** 待機判定と同期したコンシューマ位置。 */
    const LONG64 synchronized_dequeue = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
    if (detail::ShouldSignalLogConsumer(static_cast<u64>(reservation.position), static_cast<u64>(synchronized_dequeue))) {
        ::_InterlockedIncrement64(&g_state.wake_signal_count);
        ::WakeConditionVariable(&g_state.wake_cv);
    }
    ::ReleaseSRWLockShared(&g_state.wake_lock);
}

/**
 * writer thread の生成前に確保した初期化資源を巻き戻す。
 *
 * @details 呼び出し時点では writer thread が存在せず、g_inited も 0 であることを前提とする。
 *          呼び出し元が lifecycle lock を排他保持していること。
 */
void RollbackInitWithoutWriter() noexcept
{
    ::_InterlockedExchange(&g_inited, 0);
    ::_InterlockedExchange(&g_state.running, 0);

    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_state.out_file);
        g_state.out_file = INVALID_HANDLE_VALUE;
    }
    if (g_state.ring != nullptr) {
        ::VirtualFree(g_state.ring, 0, MEM_RELEASE);
        g_state.ring = nullptr;
    }

    g_state.writer_thread = nullptr;
    ::_InterlockedExchangePointer(&g_state.sink, nullptr);
    g_state.out_console = INVALID_HANDLE_VALUE;
    g_state.use_console = false;
    g_state.use_dbgout = false;
    g_state.capacity = 0;
    g_state.mask = 0;
    g_state.enqueue_pos = 0;
    g_state.dequeue_pos = 0;
    g_state.completed_emit_pos = 0;
    g_state.dropped = 0;
    g_state.wake_signal_count = 0;
}

/**
 * 2 のべき乗かを判定する。
 *
 * @param v 判定する値。
 * @return v が 0 でなく 2 のべき乗なら true。
 */
ACS_FORCEINLINE bool IsPowerOfTwo(u32 v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}

/** 設定値を桁あふれなしで許容範囲内の 2 のべき乗へ補正する。 */
u32 NormalizeRingCapacity(u32 requested_capacity) noexcept
{
    if (requested_capacity <= kMinimumRingCapacity) return kMinimumRingCapacity;
    if (requested_capacity >= kMaximumRingCapacity) return kMaximumRingCapacity;
    if (IsPowerOfTwo(requested_capacity)) return requested_capacity;

    u32 capacity = kMinimumRingCapacity;
    while (capacity < requested_capacity)
        capacity <<= 1;
    return capacity;
}

/**
 * HANDLE へ全バイトを書き出す (部分書き込みに対応してループ)。
 *
 * @param h 書き込み先ハンドル (無効ハンドルなら何もしない)。
 * @param p 書き込むバッファ。
 * @param n 書き込むバイト数。
 */
void WriteAll(HANDLE h, const char* p, usize n) noexcept
{
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
void FormatTimestamp(const LARGE_INTEGER& qpc, char* out, usize cap) noexcept
{
    LONGLONG delta = qpc.QuadPart - g_state.qpc_origin.QuadPart;
    const LONGLONG freq = g_state.qpc_freq.QuadPart > 0 ? g_state.qpc_freq.QuadPart : 1;
    // delta*10000000 を直接やると freq~10MHz では稼働 ~25h で i64 overflow するため、
    // 秒部と剰余部に分けて 100ns 単位へ変換する (どちらも overflow しない)。
    const LONGLONG sec = delta / freq;
    const LONGLONG rem = delta % freq;
    const LONGLONG ns100 = sec * 10000000LL + (rem * 10000000LL) / freq;

    ULARGE_INTEGER ft;
    ft.LowPart = g_state.ft_origin.dwLowDateTime;
    ft.HighPart = g_state.ft_origin.dwHighDateTime;
    ft.QuadPart += static_cast<ULONGLONG>(ns100);

    FILETIME ftloc;
    SYSTEMTIME st;
    FILETIME ftnow{};
    ftnow.dwLowDateTime = ft.LowPart;
    ftnow.dwHighDateTime = ft.HighPart;
    ::FileTimeToLocalFileTime(&ftnow, &ftloc);
    ::FileTimeToSystemTime(&ftloc, &st);

    ::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, st.wMilliseconds);
}

/**
 * 1 セルを 1 行に整形し、有効な全出力先 (コンソール / ファイル / デバッガ) に書き出す。
 *
 * @param c 出力するログレコード。
 */
void EmitOne(const FCell& c) noexcept
{
    char ts[32];
    FormatTimestamp(c.timestamp, ts, sizeof(ts));

    // [時刻] [レベル] [tid] file:line (func) | message
    char line[1024];
    const int n = ::snprintf(line, sizeof(line), "[%s] [%-5s] [tid=%lu] %s:%u (%s) | %.*s\n", ts, ToString(c.severity),
                             static_cast<unsigned long>(c.thread_id), c.loc.File(), c.loc.Line(), c.loc.Function(),
                             static_cast<int>(c.message_len), c.message);
    if (n < 0) return;
    const usize len = static_cast<usize>(n) < sizeof(line) ? static_cast<usize>(n) : sizeof(line) - 1;

    if (g_state.use_console) WriteAll(g_state.out_console, line, len);
    if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, line, len);
    if (g_state.use_dbgout) {
        // デバッガ (VS 出力ウィンドウ等) は UTF-16 を正しく表示する。UTF-8 → UTF-16 に
        // 変換して OutputDebugStringW で出すことで、ANSI 経由の文字化けを防ぐ。
        wchar_t wline[1024];
        const int wn = ::MultiByteToWideChar(CP_UTF8, 0, line, static_cast<int>(len), wline,
                                             static_cast<int>(sizeof(wline) / sizeof(wchar_t)) - 1);
        if (wn > 0) {
            wline[wn] = L'\0';
            ::OutputDebugStringW(wline);
        } else {
            ::OutputDebugStringA(line);
        }
    }

    // 追加シンク (エディタのログコンソール取り込み等)。callback 全体を共有側に置き、
    // SetSink の排他交換と直列化する。これにより SetSink 復帰後に旧 callback は残らず、
    // 交換後の新 callback を SetSink が誤って待つこともない。
    ::AcquireSRWLockShared(&g_sink_callback_lock);
    auto sink = reinterpret_cast<void (*)(ELogSeverity, const char*)>(
        ::_InterlockedCompareExchangePointer(&g_state.sink, nullptr, nullptr));
    if (sink != nullptr) {
        t_inside_sink_callback = true;
        sink(c.severity, c.message);
        t_inside_sink_callback = false;
    }
    ::ReleaseSRWLockShared(&g_sink_callback_lock);
}

/**
 * ライタースレッド本体。リングを空になるまでドレインし、drop 警告を出し、スリープする。
 *
 * @details running が 0 かつリングが空になったら終了する。起床は CV または 100ms タイムアウト。
 * @return スレッド終了コード (常に 0)。
 */
DWORD WINAPI WriterThreadProc(LPVOID) noexcept
{
    while (true) {
        // === ドレインループ: リングを空になるまで読み出す ===
        for (;;) {
            LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            FCell& cell = g_state.ring[pos & g_state.mask];
            LONG64 seq = ::_InterlockedExchangeAdd64(&cell.sequence, 0);
            LONG64 dif = seq - (pos + 1);
            if (dif == 0) {
                // pos のセルは commit 済み — 取り出し成功
                if (::_InterlockedCompareExchange64(&g_state.dequeue_pos, pos + 1, pos) == pos) {
                    EmitOne(cell);
                    // 次の周回のためにシーケンスを進める
                    ::_InterlockedExchange64(&cell.sequence, pos + g_state.capacity);
                    // Flush が sink callback を含む出力完了を判定できるよう、最後に公開する。
                    ::_InterlockedExchange64(&g_state.completed_emit_pos, pos + 1);
                }
            } else if (dif < 0) {
                break; // 空 — ドレイン終了
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
            continue; // まだ残っているのでドレイン継続
        }

        // === スリープ（CV 起床 or 100ms タイムアウト） ===
        // lock 取得後にもう一度空を確認する。プロデューサ側の共有 lock と組み合わせ、
        // 空判定直後から CV 待機開始までの通知取りこぼしを防ぐ。
        ::AcquireSRWLockExclusive(&g_state.wake_lock);
        /** プロデューサが次に確保する位置。 */
        const LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        /** コンシューマが次に読み取る位置。 */
        const LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
        if (::_InterlockedExchangeAdd(&g_state.running, 0) != 0 && tail >= head) {
            ::SleepConditionVariableSRW(&g_state.wake_cv, &g_state.wake_lock, 100, 0);
        }
        ::ReleaseSRWLockExclusive(&g_state.wake_lock);
    }

    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(g_state.out_file);
    }
    return 0;
}

} // 無名名前空間

/** 全資源の公開が完了し、Write を受け付けられる状態かを返す。 */
bool FLogger::IsInitialized() noexcept
{
    return ::_InterlockedExchangeAdd(&g_inited, 0) != 0;
}

/** ロガーを初期化する。詳細は宣言を参照。 */
void FLogger::Init(const FLogConfig& configuration) noexcept
{
    // writer 自身から lifecycle を切り替えると Shutdown の join と循環するため受け付けない。
    if (t_inside_sink_callback) return;

    // 初期化の全期間を終了処理と直列化する。g_inited (ready) は ring/thread を
    // 用意し終えた末尾でのみ立て、Write へ半初期化状態を公開しない。
    ::AcquireSRWLockExclusive(&g_lifecycle_lock);
    if (::_InterlockedExchangeAdd(&g_inited, 0) != 0) {
        ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
        return;
    }

    // capacity を許容範囲内の 2 のべき乗へ補正する。
    const u32 cap = NormalizeRingCapacity(configuration.ring_capacity);

    // ページ単位でリング確保
    void* const mem = ::VirtualAlloc(nullptr, sizeof(FCell) * cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        ::OutputDebugStringA("[acs::FLogger] FATAL: ring allocation failed\n");
        ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
        return;
    }
    g_state.ring = static_cast<FCell*>(mem);
    g_state.capacity = cap;
    g_state.mask = cap - 1;
    // 各セルのシーケンス番号を i 番目=i から開始
    for (u32 i = 0; i < cap; ++i) {
        g_state.ring[i].sequence = i;
    }
    g_state.enqueue_pos = 0;
    g_state.dequeue_pos = 0;
    g_state.completed_emit_pos = 0;
    g_state.dropped = 0;
    g_state.wake_signal_count = 0;

    // 出力先ハンドル取得
    g_state.use_console = configuration.console;
    g_state.use_dbgout = configuration.debug_output;
    if (configuration.console) {
        g_state.out_console = ::GetStdHandle(STD_OUTPUT_HANDLE);
        // ログ本文は UTF-8 (ソースは /utf-8 でコンパイル)。コンソールの既定コードページ
        // (日本語環境では 932 等) のままだと UTF-8 バイト列が誤解釈されて文字化けするため、
        // 出力コードページを UTF-8 (65001) に切り替える。
        ::SetConsoleOutputCP(CP_UTF8);
    }
    if (configuration.file_path) {
        g_state.out_file = ::CreateFileW(configuration.file_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        // 先頭に UTF-8 BOM を書き、テキストエディタが UTF-8 として開けるようにする。
        if (g_state.out_file != INVALID_HANDLE_VALUE) {
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            DWORD w = 0;
            ::WriteFile(g_state.out_file, bom, 3, &w, nullptr);
        }
    }

    // QPC ↔ wall clock キャリブレーション
    ::QueryPerformanceFrequency(&g_state.qpc_freq);
    ::QueryPerformanceCounter(&g_state.qpc_origin);
    ::GetSystemTimeAsFileTime(&g_state.ft_origin);

    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(configuration.min_severity));
    ::_InterlockedExchange(&g_state.running, 1);

    // ライタースレッド起動
    g_state.writer_thread = ::CreateThread(nullptr, 0, &WriterThreadProc, nullptr, 0, nullptr);
    if (g_state.writer_thread == nullptr) {
        const DWORD error = ::GetLastError();
        RollbackInitWithoutWriter();

        char message[160];
        ::snprintf(message, sizeof(message), "[acs::FLogger] FATAL: writer thread creation failed (error=%lu)\n",
                   static_cast<unsigned long>(error));
        ::OutputDebugStringA(message);
        ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
        return;
    }
    ::SetThreadDescription(g_state.writer_thread, L"acs::FLogger writer");

    // 全状態 (ring/mask/threads/calibration) を公開し終えた最後に ready を立てる。
    ::_InterlockedExchange(&g_inited, 1);
    ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

/** ライタースレッドを停止しリソースを解放する。詳細は宣言を参照。 */
void FLogger::Shutdown() noexcept
{
    // writer thread は自身を join できない。sink callback からの終了要求は所有側へ戻して行う。
    if (t_inside_sink_callback) return;

    // 二重 Shutdown と Init 完了前の Shutdown を同じ排他領域へ入れ、CloseHandle /
    // VirtualFree の所有権を常に 1 スレッドだけにする。
    ::AcquireSRWLockExclusive(&g_lifecycle_lock);
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) {
        ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
        return;
    }

    // ring 解放より先に ready を下げ、新規 producer を ring から締め出す
    // (free-before-clear だと g_inited==1 のまま解放済み ring を UAF し得る)。
    ::_InterlockedExchange(&g_inited, 0);
    ::_InterlockedIncrement(&g_lifecycle_generation);

    // ready を読む直前に停止スレッドへ割り込まれた producer も、加算後の再確認で
    // ring へ入らず退出する。既に入っている producer が全員公開/退出してから writer を止める。
    while (::_InterlockedExchangeAdd(&g_active_producers, 0) != 0) {
        ::WakeAllConditionVariable(&g_state.wake_cv);
        ::SwitchToThread();
    }
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
    // 再初期化後に破棄済みの利用側オブジェクトへ通知しない。
    ::_InterlockedExchangePointer(&g_state.sink, nullptr);
    ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

/** 残レコードを書き出すまで待つ。詳細は宣言を参照。 */
void FLogger::Flush() noexcept
{
    // callback を実行中の writer 自身が待機しても、後続レコードを処理できない。
    if (t_inside_sink_callback) return;

    // Shutdown が ring/file を解放している間は読み取らない。
    ::AcquireSRWLockShared(&g_lifecycle_lock);
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) {
        ::ReleaseSRWLockShared(&g_lifecycle_lock);
        return;
    }
    // 呼び出し後の Write を待ち続けないよう、この時点で受理済みの位置を完了目標にする。
    const LONG64 target = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);

    // 最大 1000 回リトライ（1ms x 1000 = 1秒）
    for (int i = 0; i < 1000; ++i) {
        const LONG64 completed = ::_InterlockedExchangeAdd64(&g_state.completed_emit_pos, 0);
        if (completed >= target) break;
        ::WakeAllConditionVariable(&g_state.wake_cv);
        ::Sleep(1);
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_state.out_file);
    ::ReleaseSRWLockShared(&g_lifecycle_lock);
}

void FLogger::SetMinSeverity(ELogSeverity severity) noexcept
{
    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(severity));
}

void FLogger::SetSink(void (*sink)(ELogSeverity, const char*)) noexcept
{
    if (t_inside_sink_callback) {
        // callback 自身は共有 lock を保持しているため、排他取得は自己デッドロックになる。
        // 外部の排他 SetSink は callback 完了まで入れないので、この区間の直接交換は直列化される。
        if (::_InterlockedExchangeAdd(&g_inited, 0) != 0) {
            ::_InterlockedExchangePointer(&g_state.sink, reinterpret_cast<void*>(sink));
        }
        return;
    }

    // 終了処理による sink の破棄と競合させない。非稼働中の登録は次回 lifecycle へ
    // 意図せず持ち越さないため無視する。sink の排他 lock は交換前の callback 完了を待ち、
    // 交換後の callback は lock 解放後にだけ開始できるため、旧世代だけを正確に待機できる。
    ::AcquireSRWLockExclusive(&g_lifecycle_lock);
    if (::_InterlockedExchangeAdd(&g_inited, 0) != 0) {
        ::AcquireSRWLockExclusive(&g_sink_callback_lock);
        ::_InterlockedExchangePointer(&g_state.sink, reinterpret_cast<void*>(sink));
        ::ReleaseSRWLockExclusive(&g_sink_callback_lock);
    }
    ::ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

bool FLogger::Enabled(ELogSeverity severity) noexcept
{
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return false;
    return static_cast<LONG>(severity) >= ::_InterlockedExchangeAdd(&g_state.min_severity, 0);
}

u64 FLogger::DroppedCount() noexcept
{
    // writer は g_state.dropped を周回毎に 0 化するため、累積は dropped_total を返す。
    return static_cast<u64>(::_InterlockedExchangeAdd64(&g_state.dropped_total, 0));
}

u64 FLogger::WakeSignalCount() noexcept
{
    return static_cast<u64>(::_InterlockedExchangeAdd64(&g_state.wake_signal_count, 0));
}

/** プロデューサ実体: Vyukov 風 CAS でセルを予約し、書き込んで release 公開する。詳細は宣言を参照。 */
void FLogger::Write(ELogSeverity severity, FSourceLoc location, const char* format, ...) noexcept
{
    const LONG lifecycle_generation = ::_InterlockedExchangeAdd(&g_lifecycle_generation, 0);
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    if (static_cast<LONG>(severity) < ::_InterlockedExchangeAdd(&g_state.min_severity, 0)) return;

    ::_InterlockedIncrement(&g_active_producers);
    FProducerGuard producer_guard;
    (void)producer_guard;
    // Shutdown が最初の ready 確認と producer 登録の間に始まった場合、または既に
    // 再初期化された場合は、旧 lifecycle の呼び出しを新しい ring へ持ち越さない。
    if (!::_InterlockedExchangeAdd(&g_inited, 0) ||
        lifecycle_generation != ::_InterlockedExchangeAdd(&g_lifecycle_generation, 0))
        return;

    /** 今回のログレコード用に確保したセル。 */
    FRecordReservation reservation;
    if (!ReserveRecord(severity, location, reservation)) return;

    /** printf互換の可変長引数。 */
    va_list ap;
    va_start(ap, format);
    /** 整形後の文字数。 */
    int n = ::vsnprintf(reservation.cell->message, kMessageMax, format ? format : "(null)", ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (static_cast<u32>(n) >= kMessageMax) n = static_cast<int>(kMessageMax - 1);
    reservation.cell->message_len = static_cast<u16>(n);
    PublishRecord(reservation);
}

void FLogger::WriteMessage(ELogSeverity severity, FSourceLoc location, const char* message, usize length) noexcept
{
    /** 書き込み開始時のロガー世代。 */
    const LONG lifecycle_generation = ::_InterlockedExchangeAdd(&g_lifecycle_generation, 0);
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    if (static_cast<LONG>(severity) < ::_InterlockedExchangeAdd(&g_state.min_severity, 0)) {
        return;
    }

    ::_InterlockedIncrement(&g_active_producers);
    /** 終了時に活動中プロデューサ数を戻す保護物。 */
    FProducerGuard producer_guard;
    (void)producer_guard;
    if (!::_InterlockedExchangeAdd(&g_inited, 0) || lifecycle_generation != ::_InterlockedExchangeAdd(&g_lifecycle_generation, 0)) {
        return;
    }

    /** 今回のログレコード用に確保したセル。 */
    FRecordReservation reservation;
    if (!ReserveRecord(severity, location, reservation)) return;

    /** nullptrを安全な固定文字列へ置き換えたコピー元。 */
    const char* source = message ? message : "(null)";
    /** リングセルへコピーする終端文字を除いた長さ。 */
    usize copy_length = message ? length : usize{6};
    if (copy_length >= kMessageMax) copy_length = kMessageMax - 1;
    if (copy_length > 0) {
        ::memcpy(reservation.cell->message, source, copy_length);
    }
    reservation.cell->message[copy_length] = '\0';
    reservation.cell->message_len = static_cast<u16>(copy_length);
    PublishRecord(reservation);
}

} // acs 名前空間
